#include "sceFont.h"

#include "Common/TimeUtil.h"

#include <cmath>
#include <vector>
#include <map>
#include <algorithm>

#include "Common/Serialize/Serializer.h"
#include "Common/Serialize/SerializeFuncs.h"
#include "Common/Serialize/SerializeMap.h"
#include "Core/HLE/HLE.h"
#include "Core/HLE/FunctionWrappers.h"
#include "Core/HLE/sceFont.h"
#include "Core/HLE/sceKernel.h"
#include "Core/HLE/sceKernelThread.h"
#include "Core/MIPS/MIPS.h"
#include "Core/FileSystems/FileSystem.h"
#include "Core/FileSystems/MetaFileSystem.h"
#include "Core/MemMapHelpers.h"
#include "Core/Reporting.h"
#include "Core/System.h"
#include "Core/Font/PGF.h"

enum {
	ERROR_FONT_OUT_OF_MEMORY        = 0x80460001,
	ERROR_FONT_INVALID_LIBID        = 0x80460002,
	ERROR_FONT_INVALID_PARAMETER    = 0x80460003,
	ERROR_FONT_HANDLER_OPEN_FAILED  = 0x80460005,
	ERROR_FONT_TOO_MANY_OPEN_FONTS  = 0x80460009,
	ERROR_FONT_INVALID_FONT_DATA    = 0x8046000a,
};

enum {
	FONT_IS_CLOSED = 0,
	FONT_IS_OPEN   = 1,
};

// For the save states.
static bool useAllocCallbacks = true;

// Actions
static int actionPostAllocCallback;
static int actionPostOpenCallback;
static int actionPostOpenAllocCallback;
static int actionPostCharInfoAllocCallback;
static int actionPostCharInfoFreeCallback;

// Monster Hunter sequence:
// 36:46:998 c:\dev\ppsspp\core\hle\scefont.cpp:469 E[HLE]: sceFontNewLib 89ad4a0, 9fff5cc
// 36:46:998 c:\dev\ppsspp\core\hle\scefont.cpp:699 E[HLE]: UNIMPL sceFontGetNumFontList 1, 9fff5cc
// 36:46:998 c:\dev\ppsspp\core\hle\scefont.cpp:526 E[HLE]: sceFontFindOptimumFont 1, 9fff524, 9fff5cc
// 36:46:999 c:\dev\ppsspp\core\hle\scefont.cpp:490 E[HLE]: sceFontOpenFont 1, 1, 0, 9fff5cc
// 36:46:999 c:\dev\ppsspp\core\hle\scefont.cpp:542 E[HLE]: sceFontGetFontInfo 1, 997140c

typedef u32 FontLibraryHandle;
typedef u32 FontHandle;

struct FontNewLibParams {
	u32_le userDataAddr;
	u32_le numFonts;
	u32_le cacheDataAddr;

	// Driver callbacks.
	u32_le allocFuncAddr;
	u32_le freeFuncAddr;
	u32_le openFuncAddr;
	u32_le closeFuncAddr;
	u32_le readFuncAddr;
	u32_le seekFuncAddr;
	u32_le errorFuncAddr;
	u32_le ioFinishFuncAddr;
};

struct FontRegistryEntry {
	int hSize;
	int vSize;
	int hResolution;
	int vResolution;
	int extraAttributes;
	int weight;
	int familyCode;
	int style;
	int styleSub;
	int languageCode;
	int regionCode;
	int countryCode;
	const char *fileName;
	const char *fontName;
	int expireDate;
	int shadow_option;
	u32 fontFileSize;
	u32 stingySize; // for the FONT_OPEN_INTERNAL_STINGY mode, from pspautotests.
	bool ignoreIfMissing;
};

static const FontRegistryEntry fontRegistry[] = {
	// This was added for Chinese translations and is not normally loaded on a PSP.
	{ 0x288, 0x288, 0x2000, 0x2000, 0, 0, FONT_FAMILY_SANS_SERIF, FONT_STYLE_DB, 0, FONT_LANGUAGE_CHINESE, 0, 1, "zh_gb.pgf", "FTT-NewRodin Pro DB", 0, 0, 1581700, 145844, true },
	{ 0x288, 0x288, 0x2000, 0x2000, 0, 0, FONT_FAMILY_SANS_SERIF, FONT_STYLE_DB, 0, FONT_LANGUAGE_JAPANESE, 0, 1, "jpn0.pgf", "FTT-NewRodin Pro DB", 0, 0, 1581700, 145844 },
	{ 0x288, 0x288, 0x2000, 0x2000, 0, 0, FONT_FAMILY_SANS_SERIF, FONT_STYLE_REGULAR, 0, FONT_LANGUAGE_LATIN, 0, 1, "ltn0.pgf", "FTT-NewRodin Pro Latin", 0, 0, 69108, 16680 },
	{ 0x288, 0x288, 0x2000, 0x2000, 0, 0, FONT_FAMILY_SERIF, FONT_STYLE_REGULAR, 0, FONT_LANGUAGE_LATIN, 0, 1, "ltn1.pgf", "FTT-Matisse Pro Latin", 0, 0, 65124, 16920 },
	{ 0x288, 0x288, 0x2000, 0x2000, 0, 0, FONT_FAMILY_SANS_SERIF, FONT_STYLE_ITALIC, 0, FONT_LANGUAGE_LATIN, 0, 1, "ltn2.pgf", "FTT-NewRodin Pro Latin", 0, 0, 72948, 16872 },
	{ 0x288, 0x288, 0x2000, 0x2000, 0, 0, FONT_FAMILY_SERIF, FONT_STYLE_ITALIC, 0, FONT_LANGUAGE_LATIN, 0, 1, "ltn3.pgf", "FTT-Matisse Pro Latin", 0, 0, 67700, 17112 },
	{ 0x288, 0x288, 0x2000, 0x2000, 0, 0, FONT_FAMILY_SANS_SERIF, FONT_STYLE_BOLD, 0, FONT_LANGUAGE_LATIN, 0, 1, "ltn4.pgf", "FTT-NewRodin Pro Latin", 0, 0, 72828, 16648 },
	{ 0x288, 0x288, 0x2000, 0x2000, 0, 0, FONT_FAMILY_SERIF, FONT_STYLE_BOLD, 0, FONT_LANGUAGE_LATIN, 0, 1, "ltn5.pgf", "FTT-Matisse Pro Latin", 0, 0, 68220, 16928 },
	{ 0x288, 0x288, 0x2000, 0x2000, 0, 0, FONT_FAMILY_SANS_SERIF, FONT_STYLE_BOLD_ITALIC, 0, FONT_LANGUAGE_LATIN, 0, 1, "ltn6.pgf", "FTT-NewRodin Pro Latin", 0, 0, 77032, 16792 },
	{ 0x288, 0x288, 0x2000, 0x2000, 0, 0, FONT_FAMILY_SERIF, FONT_STYLE_BOLD_ITALIC, 0, FONT_LANGUAGE_LATIN, 0, 1, "ltn7.pgf", "FTT-Matisse Pro Latin", 0, 0, 71144, 17160 },
	{ 0x1c0, 0x1c0, 0x2000, 0x2000, 0, 0, FONT_FAMILY_SANS_SERIF, FONT_STYLE_REGULAR, 0, FONT_LANGUAGE_LATIN, 0, 1, "ltn8.pgf", "FTT-NewRodin Pro Latin", 0, 0, 41000, 16192 },
	{ 0x1c0, 0x1c0, 0x2000, 0x2000, 0, 0, FONT_FAMILY_SERIF, FONT_STYLE_REGULAR, 0, FONT_LANGUAGE_LATIN, 0, 1, "ltn9.pgf", "FTT-Matisse Pro Latin", 0, 0, 40164, 16476 },
	{ 0x1c0, 0x1c0, 0x2000, 0x2000, 0, 0, FONT_FAMILY_SANS_SERIF, FONT_STYLE_ITALIC, 0, FONT_LANGUAGE_LATIN, 0, 1, "ltn10.pgf", "FTT-NewRodin Pro Latin", 0, 0, 42692, 16300 },
	{ 0x1c0, 0x1c0, 0x2000, 0x2000, 0, 0, FONT_FAMILY_SERIF, FONT_STYLE_ITALIC, 0, FONT_LANGUAGE_LATIN, 0, 1, "ltn11.pgf", "FTT-Matisse Pro Latin", 0, 0, 41488, 16656 },
	{ 0x1c0, 0x1c0, 0x2000, 0x2000, 0, 0, FONT_FAMILY_SANS_SERIF, FONT_STYLE_BOLD, 0, FONT_LANGUAGE_LATIN, 0, 1, "ltn12.pgf", "FTT-NewRodin Pro Latin", 0, 0, 43136, 16176 },
	{ 0x1c0, 0x1c0, 0x2000, 0x2000, 0, 0, FONT_FAMILY_SERIF, FONT_STYLE_BOLD, 0, FONT_LANGUAGE_LATIN, 0, 1, "ltn13.pgf", "FTT-Matisse Pro Latin", 0, 0, 41772, 16436 },
	{ 0x1c0, 0x1c0, 0x2000, 0x2000, 0, 0, FONT_FAMILY_SANS_SERIF, FONT_STYLE_BOLD_ITALIC, 0, FONT_LANGUAGE_LATIN, 0, 1, "ltn14.pgf", "FTT-NewRodin Pro Latin", 0, 0, 45184, 16272 },
	{ 0x1c0, 0x1c0, 0x2000, 0x2000, 0, 0, FONT_FAMILY_SERIF, FONT_STYLE_BOLD_ITALIC, 0, FONT_LANGUAGE_LATIN, 0, 1, "ltn15.pgf", "FTT-Matisse Pro Latin", 0, 0, 43044, 16704 },
	{ 0x288, 0x288, 0x2000, 0x2000, 0, 0, FONT_FAMILY_SANS_SERIF, FONT_STYLE_REGULAR, 0, FONT_LANGUAGE_KOREAN, 0, 3, "kr0.pgf", "AsiaNHH(512Johab)", 0, 0, 394192, 51856 },
};

static const float pointDPI = 72.f;

class LoadedFont;
class FontLib;
class Font;
int GetInternalFontIndex(Font *font);

// These should not need to be state saved.
static std::vector<Font *> internalFonts;
// However, these we must save - but we could take a shortcut
// for LoadedFonts that point to internal fonts.
static std::map<u32, LoadedFont *> fontMap;
static std::map<u32, u32> fontLibMap;
// We keep this list to avoid ptr references, even before alloc is called.
static std::vector<FontLib *> fontLibList;

enum MatchQuality {
	MATCH_UNKNOWN,
	MATCH_NONE,
	MATCH_GOOD,
};

enum FontOpenMode {
	FONT_OPEN_INTERNAL_STINGY   = 0,
	FONT_OPEN_INTERNAL_FULL     = 1,
	// Calls open/seek/read/close handlers to read the file partially.
	FONT_OPEN_USERFILE_HANDLERS = 2,
	// Reads directly from filesystem.
	FONT_OPEN_USERFILE_FULL     = 3,
	FONT_OPEN_USERBUFFER        = 4,
};

// TODO: Merge this class with PGF? That'd make it harder to support .bwfon
// fonts though, unless that's added directly to PGF.
class Font {
public:
	// For savestates only.
	Font() {
	}

	Font(const u8 *data, size_t dataSize) {
		Init(data, dataSize);
	}

	Font(const u8 *data, size_t dataSize, const FontRegistryEntry &entry) {
		Init(data, dataSize, entry);
	}

	Font(const std::vector<u8> &data) {
		Init(&data[0], data.size());
	}

	Font(const std::vector<u8> &data, const FontRegistryEntry &entry) {
		Init(&data[0], data.size(), entry);
	}

	const PGFFontStyle &GetFontStyle() const { return style_; }

	MatchQuality MatchesStyle(const PGFFontStyle &style) const {
		// If no field matches, it doesn't match.
		MatchQuality match = MATCH_UNKNOWN;

#define CHECK_FIELD(f, m) \
		if (style.f != 0) { \
			if (style.f != style_.f) { \
				return MATCH_NONE; \
			} \
			if (match < m) { \
				match = m; \
			} \
		}
#define CHECK_FIELD_STR(f, m) \
		if (style.f[0] != '\0') { \
			if (strcmp(style.f, style_.f) != 0) { \
				return MATCH_NONE; \
			} \
			if (match < m) { \
				match = m; \
			} \
		}

		CHECK_FIELD(fontFamily, MATCH_GOOD);
		CHECK_FIELD(fontStyle, MATCH_GOOD);
		CHECK_FIELD(fontLanguage, MATCH_GOOD);
		CHECK_FIELD(fontCountry, MATCH_GOOD);

		CHECK_FIELD_STR(fontName, MATCH_GOOD);
		CHECK_FIELD_STR(fontFileName, MATCH_GOOD);

#undef CHECK_FIELD_STR
#undef CHECK_FIELD
		return match;
	}

	PGF *GetPGF() { return &pgf_; }
	const PGF *GetPGF() const { return &pgf_; }
	u32 getSize() const { return dataSize_; }
	u32 getStingySize() const { return stingySize_; }
	bool IsValid() const { return valid_; }

	void DoState(PointerWrap &p) {
		auto s = p.Section("Font", 1, 2);
		if (!s)
			return;

		Do(p, pgf_);
		Do(p, style_);
		if (s < 2) {
			valid_ = true;
		} else {
			Do(p, valid_);
		}
	}

private:
	void Init(const u8 *data, size_t dataSize) {
		valid_ = pgf_.ReadPtr(data, dataSize);
		memset(&style_, 0, sizeof(style_));
		style_.fontH = (float)pgf_.header.hSize / 64.0f;
		style_.fontV = (float)pgf_.header.vSize / 64.0f;
		style_.fontHRes = (float)pgf_.header.hResolution / 64.0f;
		style_.fontVRes = (float)pgf_.header.vResolution / 64.0f;
		this->dataSize_ = (u32)dataSize;
		this->stingySize_ = 0; // Unused
	}

	void Init(const u8 *data, size_t dataSize, const FontRegistryEntry &entry) {
		valid_ = pgf_.ReadPtr(data, dataSize);
		style_.fontH = entry.hSize / 64.f;
		style_.fontV = entry.vSize / 64.f;
		style_.fontHRes = entry.hResolution / 64.f;
		style_.fontVRes = entry.vResolution / 64.f;
		style_.fontWeight = (float)entry.weight;
		style_.fontFamily = (u16)entry.familyCode;
		style_.fontStyle = (u16)entry.style;
		style_.fontStyleSub = (u16)entry.styleSub;
		style_.fontLanguage = (u16)entry.languageCode;
		style_.fontRegion = (u16)entry.regionCode;
		style_.fontCountry = (u16)entry.countryCode;
		strncpy(style_.fontName, entry.fontName, sizeof(style_.fontName));
		strncpy(style_.fontFileName, entry.fileName, sizeof(style_.fontFileName));
		style_.fontAttributes = entry.extraAttributes;
		style_.fontExpire = entry.expireDate;
		this->dataSize_ = entry.fontFileSize;
		this->stingySize_ = entry.stingySize;
	}

	PGF pgf_;
	PGFFontStyle style_;
	bool valid_;
	u32 dataSize_;
	u32 stingySize_;
	DISALLOW_COPY_AND_ASSIGN(Font);
};

class LoadedFont {
public:
	// For savestates only.
	LoadedFont() : font_(NULL) {
	}

	LoadedFont(Font *font, FontOpenMode mode, u32 fontLibID, u32 handle)
		: fontLibID_(fontLibID), font_(font), handle_(handle), mode_(mode), open_(true) {}

	~LoadedFont() {
		switch (mode_) {
		case FONT_OPEN_USERBUFFER:
		case FONT_OPEN_USERFILE_FULL:
		case FONT_OPEN_USERFILE_HANDLERS:
			// For these types, it's our responsibility to delete.
			delete font_;
			font_ = NULL;
			break;
		default:
			// Otherwise, it's an internal font, we keep those.
			break;
		}
	}

	const Font *GetFont() const { return font_; }
	const PGF *GetPGF() const { return font_->GetPGF(); }
	const FontLib *GetFontLib() const { return fontLibList[fontLibID_]; }
	FontLib *GetFontLib() { return fontLibList[fontLibID_]; }
	u32 Handle() const { return handle_; }

	bool GetCharInfo(int charCode, PGFCharInfo *charInfo, int glyphType = FONT_PGF_CHARGLYPH) const;
	void DrawCharacter(const GlyphImage *image, int clipX, int clipY, int clipWidth, int clipHeight, int charCode, int glyphType) const;

	bool IsOpen() const { return open_; }
	void Close() {
		open_ = false;
		// We keep the rest around until deleted, as some queries are allowed
		// on closed fonts (which is rather strange).
	}

	void DoState(PointerWrap &p) {
		auto s = p.Section("LoadedFont", 1, 3);
		if (!s)
			return;

		int numInternalFonts = (int)internalFonts.size();
		Do(p, numInternalFonts);
		if (numInternalFonts != (int)internalFonts.size()) {
			ERROR_LOG(SCEFONT, "Unable to load state: different internal font count.");
			p.SetError(p.ERROR_FAILURE);
			return;
		}

		Do(p, fontLibID_);
		int internalFont = GetInternalFontIndex(font_);
		Do(p, internalFont);
		if (internalFont == -1) {
			Do(p, font_);
		} else if (p.mode == p.MODE_READ) {
			font_ = internalFonts[internalFont];
		}
		Do(p, handle_);
		if (s >= 2) {
			Do(p, open_);
		} else {
			open_ = fontLibID_ != (u32)-1;
		}
		if (s >= 3) {
			Do(p, mode_);
		} else {
			mode_ = FONT_OPEN_INTERNAL_FULL;
		}
	}

private:
	u32 fontLibID_;
	Font *font_;
	u32 handle_;
	FontOpenMode mode_;
	bool open_;
	DISALLOW_COPY_AND_ASSIGN(LoadedFont);
};

class PostAllocCallback : public PSPAction {
public:
	PostAllocCallback() {}
	static PSPAction *Create() { return new PostAllocCallback(); }
	void DoState(PointerWrap &p) override {
		auto s = p.Section("PostAllocCallback", 1, 2);
		if (!s)
			return;

		Do(p, fontLibID_);
		if (s >= 2) {
			Do(p, errorCodePtr_);
		} else {
			errorCodePtr_ = 0;
		}
	}
	void run(MipsCall &call) override;
	void SetFontLib(u32 fontLibID, u32 errorCodePtr) { fontLibID_ = fontLibID; errorCodePtr_ = errorCodePtr; }

private:
	u32 fontLibID_;
	u32 errorCodePtr_;
};

class PostOpenCallback : public PSPAction {
public:
	PostOpenCallback() {}
	static PSPAction *Create() { return new PostOpenCallback(); }
	void DoState(PointerWrap &p) override {
		auto s = p.Section("PostOpenCallback", 1);
		if (!s)
			return;

		Do(p, fontLibID_);
	}
	void run(MipsCall &call) override;
	void SetFontLib(u32 fontLibID) { fontLibID_ = fontLibID; }

private:
	u32 fontLibID_;
};

class PostOpenAllocCallback : public PSPAction {
public:
	PostOpenAllocCallback() {}
	static PSPAction *Create() { return new PostOpenAllocCallback(); }
	void DoState(PointerWrap &p) override {
		auto s = p.Section("PostOpenAllocCallback", 1);
		if (!s)
			return;

		Do(p, fontLibID_);
		Do(p, fontHandle_);
		Do(p, fontIndex_);
	}
	void run(MipsCall &call) override;
	void SetFontLib(u32 fontLibID) { fontLibID_ = fontLibID; }
	void SetFont(u32 handle, int index) { fontHandle_ = handle; fontIndex_ = index; }

private:
	u32 fontLibID_;
	u32 fontHandle_;
	int fontIndex_;
};

class PostCharInfoAllocCallback : public PSPAction {
public:
	PostCharInfoAllocCallback() {}
	static PSPAction *Create() { return new PostCharInfoAllocCallback(); }
	void DoState(PointerWrap &p) override {
		auto s = p.Section("PostCharInfoAllocCallback", 1);
		if (!s)
			return;

		Do(p, fontLibID_);
	}
	void run(MipsCall &call) override;
	void SetFontLib(u32 fontLibID) { fontLibID_ = fontLibID; }

private:
	u32 fontLibID_;
};

class PostCharInfoFreeCallback : public PSPAction {
public:
	PostCharInfoFreeCallback() {}
	static PSPAction *Create() { return new PostCharInfoFreeCallback(); }
	void DoState(PointerWrap &p) override {
		auto s = p.Section("PostCharInfoFreeCallback", 1);
		if (!s)
			return;

		Do(p, fontLibID_);
		Do(p, charInfo_);
	}
	void run(MipsCall &call) override;
	void SetFontLib(u32 fontLibID) { fontLibID_ = fontLibID; }
	void SetCharInfo(PSPPointer<PGFCharInfo> charInfo) { charInfo_ = charInfo; }

private:
	u32 fontLibID_;
	PSPPointer<PGFCharInfo> charInfo_;
};


struct NativeFontLib {
	FontNewLibParams params;
	// TODO
	u32_le fontInfo1;
	u32_le fontInfo2;
	u16_le unk1;
	u16_le unk2;
	float_le hRes;
	float_le vRes;
	u32_le internalFontCount;
	u32_le internalFontInfo;
	u16_le altCharCode;
	u16_le unk5;
};

struct FontImageRect {
	s16_le width;
	s16_le height;
};

// A "fontLib" is a container of loaded fonts.
// One can open either "internal" fonts or custom fonts into a fontlib.
class FontLib {
public:
	FontLib() {
		// For save states only.
	}

	FontLib(u32 paramPtr, u32 errorCodePtr) : fontHRes_(128.0f), fontVRes_(128.0f), altCharCode_(0x5F), charInfoBitmapAddress_(0) {
		nfl_ = 0;
		Memory::ReadStruct(paramPtr, &params_);
		if (params_.numFonts > 9) {
			params_.numFonts = 9;
		}

		// Technically, this should be four separate allocations.
		u32 allocSize = 0x4C + params_.numFonts * 0x4C + params_.numFonts * 0x230 + (u32)internalFonts.size() * 0xA8;
		PostAllocCallback *action = (PostAllocCallback *) __KernelCreateAction(actionPostAllocCallback);
		action->SetFontLib(GetListID(), errorCodePtr);

		u32 args[2] = { userDataAddr(), allocSize };
		hleEnqueueCall(allocFuncAddr(), 2, args, action);
	}

	u32 GetListID() {
		return (u32)(std::find(fontLibList.begin(), fontLibList.end(), this) - fontLibList.begin());
	}

	void Done() {
		for (size_t i = 0; i < fonts_.size(); i++) {
			if (isfontopen_[i] == FONT_IS_OPEN) {
				CloseFont(fontMap[fonts_[i]]);
				delete fontMap[fonts_[i]];
				fontMap.erase(fonts_[i]);
			}
		}
		u32 args[2] = { userDataAddr(), (u32)handle_ };
		// TODO: The return value of this is leaking.
		if (handle_) {  // Avoid calling free-callback on double-free
			if (coreState != CORE_POWERDOWN) {
				hleEnqueueCall(freeFuncAddr(), 2, args);
			}
		}
		handle_ = 0;
		fonts_.clear();
		isfontopen_.clear();
		openAllocatedAddresses_.clear();
	}

	void AllocDone(u32 allocatedAddr) {
		handle_ = allocatedAddr;
		fonts_.resize(params_.numFonts);
		isfontopen_.resize(params_.numFonts);
		openAllocatedAddresses_.resize(params_.numFonts);
		for (size_t i = 0; i < fonts_.size(); i++) {
			u32 addr = allocatedAddr + 0x4C + (u32)i * 0x4C;
			isfontopen_[i] = 0;
			fonts_[i] = addr;
		}

		// Let's write out the native struct to make tests easier.
		// It's possible games may depend on this staying in ram, e.g. copying it, we may move to that.
		nfl_ = allocatedAddr;
		nfl_->params = params_;
		nfl_->fontInfo1 = allocatedAddr + 0x4C;
		nfl_->fontInfo2 = allocatedAddr + 0x4C + params_.numFonts * 0x4C;
		nfl_->unk1 = 0;
		nfl_->unk2 = 0;
		nfl_->hRes = fontHRes_;
		nfl_->vRes = fontVRes_;
		nfl_->internalFontCount = (u32)internalFonts.size();
		nfl_->internalFontInfo = allocatedAddr + 0x4C + params_.numFonts * 0x4C + params_.numFonts * 0x230;
		nfl_->altCharCode = altCharCode_;
	}

	u32 handle() const { return handle_; }
	int numFonts() const { return params_.numFonts; }
	u32_le userDataAddr() const{ return params_.userDataAddr; }
	u32_le allocFuncAddr() const { return params_.allocFuncAddr; }
	u32_le freeFuncAddr() const { return params_.freeFuncAddr; }

	void SetResolution(float hres, float vres) {
		fontHRes_ = hres;
		fontVRes_ = vres;
		if (nfl_.IsValid()) {
			nfl_->hRes = hres;
			nfl_->vRes = vres;
		}
	}

	float FontHRes() const { return fontHRes_; }
	float FontVRes() const { return fontVRes_; }

	void SetAltCharCode(int charCode) {
		altCharCode_ = charCode;
		if (nfl_.IsValid())
			nfl_->altCharCode = charCode;
	}

	int GetFontHandle(int index) const {
		return fonts_[index];
	}

	// For FONT_OPEN_USER* modes, the font will automatically be freed.
	LoadedFont *OpenFont(Font *font, FontOpenMode mode, int &error) {
		// TODO: Do something with mode, possibly save it where the PSP does in the struct.
		// Maybe needed in Font, though?  Handlers seem... difficult to emulate.
		int freeFontIndex = -1;
		for (size_t i = 0; i < fonts_.size(); i++) {
			if (isfontopen_[i] == 0) {
				freeFontIndex = (int)i;
				break;
			}
		}
		if (freeFontIndex < 0) {
			ERROR_LOG(SCEFONT, "Too many fonts opened in FontLib");
			error = ERROR_FONT_TOO_MANY_OPEN_FONTS;
			return 0;
		}
		if (!font->IsValid()) {
			ERROR_LOG(SCEFONT, "Invalid font data");
			error = ERROR_FONT_INVALID_FONT_DATA;
			return 0;
		}
		LoadedFont *loadedFont = new LoadedFont(font, mode, GetListID(), fonts_[freeFontIndex]);
		isfontopen_[freeFontIndex] = 1;

		auto prevFont = fontMap.find(loadedFont->Handle());
		if (prevFont != fontMap.end()) {
			// Before replacing it and forgetting about it, let's free it.
			delete prevFont->second;
		}
		fontMap[loadedFont->Handle()] = loadedFont;

		if (!useAllocCallbacks)
			return loadedFont;

		u32 allocSize = 12;
		if (mode == FONT_OPEN_INTERNAL_STINGY) {
			allocSize = loadedFont->GetFont()->getStingySize();
		} else if (mode == FONT_OPEN_INTERNAL_FULL) {
			allocSize += loadedFont->GetFont()->getSize();
		}

		PostOpenAllocCallback *action = (PostOpenAllocCallback *)__KernelCreateAction(actionPostOpenAllocCallback);
		action->SetFontLib(GetListID());
		action->SetFont(loadedFont->Handle(), freeFontIndex);

		u32 args[2] = { userDataAddr(), allocSize };
		hleEnqueueCall(allocFuncAddr(), 2, args, action);

		return loadedFont;
	}

	void CloseFont(LoadedFont *font) {
		for (size_t i = 0; i < fonts_.size(); i++) {
			if (fonts_[i] == font->Handle()) {
				isfontopen_[i] = 0;
				if (openAllocatedAddresses_[i] != 0 && coreState != CORE_POWERDOWN) {
					u32 args[2] = { userDataAddr(), openAllocatedAddresses_[i] };
					hleEnqueueCall(freeFuncAddr(), 2, args);
					openAllocatedAddresses_[i] = 0;
				}
				break;
			}
		}
		flushFont();
		font->Close();
	}

	void flushFont() {
		if (charInfoBitmapAddress_ != 0 && coreState != CORE_POWERDOWN) {
			u32 args[2] = { userDataAddr(), charInfoBitmapAddress_ };
			hleEnqueueCall(freeFuncAddr(), 2, args);
			charInfoBitmapAddress_ = 0;
		}
	}

	void DoState(PointerWrap &p) {
		auto s = p.Section("FontLib", 1, 3);
		if (!s)
			return;

		Do(p, fonts_);
		Do(p, isfontopen_);
		Do(p, params_);
		Do(p, fontHRes_);
		Do(p, fontVRes_);
		Do(p, fileFontHandle_);
		Do(p, handle_);
		Do(p, altCharCode_);
		if (s >= 2) {
			Do(p, nfl_);
		} else {
			nfl_ = 0;
		}

		if (s >= 3) {
			Do(p, openAllocatedAddresses_);
			Do(p, charInfoBitmapAddress_);
		} else {
			openAllocatedAddresses_.resize(params_.numFonts);
			charInfoBitmapAddress_ = 0;
		}
	}

	void SetFileFontHandle(u32 handle) {
		fileFontHandle_ = handle;
	}

	u32 GetAltCharCode() const { return altCharCode_; }

	u32 GetOpenAllocatedAddress(int index) const { 
		if(index < numFonts())
			return openAllocatedAddresses_[index];
		return 0;
	}

	void SetOpenAllocatedAddress(int index, u32 addr) {
		if (index < numFonts())
			openAllocatedAddresses_[index] = addr;
	}

	u32 GetCharInfoBitmapAddress() const { return charInfoBitmapAddress_; }
	void SetCharInfoBitmapAddress(u32 addr) { charInfoBitmapAddress_ = addr; }

private:
	std::vector<u32> fonts_;
	std::vector<u32> isfontopen_;

	FontNewLibParams params_;
	float fontHRes_;
	float fontVRes_;
	int fileFontHandle_;
	int handle_;
	int altCharCode_;
	std::vector<u32> openAllocatedAddresses_;
	u32 charInfoBitmapAddress_;
	PSPPointer<NativeFontLib> nfl_;

	DISALLOW_COPY_AND_ASSIGN(FontLib);
};


void PostAllocCallback::run(MipsCall &call) {
	INFO_LOG(SCEFONT, "Entering PostAllocCallback::run");
	u32 v0 = currentMIPS->r[MIPS_REG_V0];
	if (v0 == 0) {
		// TODO: Who deletes fontLib?
		if (errorCodePtr_)
			Memory::Write_U32(ERROR_FONT_OUT_OF_MEMORY, errorCodePtr_);
		call.setReturnValue(0);
	} else {
		FontLib *fontLib = fontLibList[fontLibID_];
		fontLib->AllocDone(v0);
		fontLibMap[fontLib->handle()] = fontLibID_;
		// This is the same as v0 above.
		call.setReturnValue(fontLib->handle());
	}
	INFO_LOG(SCEFONT, "Leaving PostAllocCallback::run");
}

void PostOpenCallback::run(MipsCall &call) {
	FontLib *fontLib = fontLibList[fontLibID_];
	u32 v0 = currentMIPS->r[MIPS_REG_V0];
	fontLib->SetFileFontHandle(v0);
}

void PostOpenAllocCallback::run(MipsCall &call) {
	FontLib *fontLib = fontLibList[fontLibID_];
	u32 v0 = currentMIPS->r[MIPS_REG_V0];
	fontLib->SetOpenAllocatedAddress(fontIndex_, v0);
}

void PostCharInfoAllocCallback::run(MipsCall &call) {
	FontLib *fontLib = fontLibList[fontLibID_];
	u32 v0 = currentMIPS->r[MIPS_REG_V0];
	if (v0 == 0) {
		call.setReturnValue(ERROR_FONT_OUT_OF_MEMORY); // From JPCSP, if alloc size is 0, still this error value?
	} else {
		fontLib->SetCharInfoBitmapAddress(v0);
	}
}

void PostCharInfoFreeCallback::run(MipsCall &call) {
	FontLib *fontLib = fontLibList[fontLibID_];
	fontLib->SetCharInfoBitmapAddress(0);

	u32 allocSize = charInfo_->bitmapWidth * charInfo_->bitmapHeight;
	PostCharInfoAllocCallback *action = (PostCharInfoAllocCallback *)__KernelCreateAction(actionPostCharInfoAllocCallback);
	action->SetFontLib(fontLibID_);

	u32 args[2] = { fontLib->userDataAddr(), allocSize };
	hleEnqueueCall(fontLib->allocFuncAddr(), 2, args, action);
}

inline bool LoadedFont::GetCharInfo(int charCode, PGFCharInfo *charInfo, int glyphType) const {
	auto fontLib = GetFontLib();
	int altCharCode = fontLib == NULL ? -1 : fontLib->GetAltCharCode();
	return GetPGF()->GetCharInfo(charCode, charInfo, altCharCode, glyphType);
}

inline void LoadedFont::DrawCharacter(const GlyphImage *image, int clipX, int clipY, int clipWidth, int clipHeight, int charCode, int glyphType) const {
	auto fontLib = GetFontLib();
	int altCharCode = fontLib == NULL ? -1 : fontLib->GetAltCharCode();
	GetPGF()->DrawCharacter(image, clipX, clipY, clipWidth, clipHeight, charCode, altCharCode, glyphType);
}

static FontLib *GetFontLib(u32 handle) {
	if (fontLibMap.find(handle) != fontLibMap.end()) {
		return fontLibList[fontLibMap[handle]];
	} else {
		ERROR_LOG(SCEFONT, "No fontlib with handle %08x", handle);
		return 0;
	}
}

static LoadedFont *GetLoadedFont(u32 handle, bool allowClosed) {
	auto iter = fontMap.find(handle);
	if (iter != fontMap.end()) {
		if (iter->second->IsOpen() || allowClosed) {
			return fontMap[handle];
		} else {
			ERROR_LOG(SCEFONT, "Font exists but is closed, which was not allowed in this call.");
			return 0;
		}
	} else {
		ERROR_LOG(SCEFONT, "No font with handle %08x", handle);
		return 0;
	}
}

static void __LoadInternalFonts() {
	if (internalFonts.size()) {
		// Fonts already loaded.
		return;
	}
	const std::string fontPath = "flash0:/font/";
	const std::string fontOverridePath = "ms0:/PSP/flash0/font/";
	const std::string userfontPath = "disc0:/PSP_GAME/USRDIR/";
	
	if (!pspFileSystem.GetFileInfo(fontPath).exists) {
		pspFileSystem.MkDir(fontPath);
	}
	if ((pspFileSystem.GetFileInfo("disc0:/PSP_GAME/USRDIR/zh_gb.pgf").exists) && (pspFileSystem.GetFileInfo("disc0:/PSP_GAME/USRDIR/oldfont.prx").exists)) {
		for (size_t i = 0; i < ARRAY_SIZE(fontRegistry); i++) {
			const FontRegistryEntry &entry = fontRegistry[i];
			std::string fontFilename = userfontPath + entry.fileName;
			PSPFileInfo info = pspFileSystem.GetFileInfo(fontFilename);
			DEBUG_LOG(SCEFONT, "Loading internal font %s (%i bytes)", fontFilename.c_str(), (int)info.size);
			std::vector<u8> buffer;
			if (pspFileSystem.ReadEntireFile(fontFilename, buffer) < 0) {
				ERROR_LOG(SCEFONT, "Failed opening font");
				continue;
			}
			internalFonts.push_back(new Font(buffer, entry));
			DEBUG_LOG(SCEFONT, "Loaded font %s", fontFilename.c_str());
			return;
		}
	}

	for (size_t i = 0; i < ARRAY_SIZE(fontRegistry); i++) {
		const FontRegistryEntry &entry = fontRegistry[i];
		std::string fontFilename = userfontPath + entry.fileName;
		PSPFileInfo info = pspFileSystem.GetFileInfo(fontFilename);

		if (!info.exists) {
			// No user font, let's try override path.
			fontFilename = fontOverridePath + entry.fileName;
			info = pspFileSystem.GetFileInfo(fontFilename);
		}

		if (!info.exists) {
			// No override, let's use the default path.
			fontFilename = fontPath + entry.fileName;
			info = pspFileSystem.GetFileInfo(fontFilename);
		}

		if (info.exists) {
			DEBUG_LOG(SCEFONT, "Loading internal font %s (%i bytes)", fontFilename.c_str(), (int)info.size);
			std::vector<u8> buffer;
			if (pspFileSystem.ReadEntireFile(fontFilename, buffer) < 0) {
				ERROR_LOG(SCEFONT, "Failed opening font");
				continue;
			}
			
			internalFonts.push_back(new Font(buffer, entry));

			DEBUG_LOG(SCEFONT, "Loaded font %s", fontFilename.c_str());
		} else if (!entry.ignoreIfMissing) {
			WARN_LOG(SCEFONT, "Font file not found: %s", fontFilename.c_str());
		}
	}
}

int GetInternalFontIndex(Font *font) {
	for (size_t i = 0; i < internalFonts.size(); i++) {
		if (internalFonts[i] == font)
			return (int)i;
	}
	return -1;
}

void __FontInit() {
	useAllocCallbacks = true;
	actionPostAllocCallback = __KernelRegisterActionType(PostAllocCallback::Create);
	actionPostOpenCallback = __KernelRegisterActionType(PostOpenCallback::Create);
	actionPostOpenAllocCallback = __KernelRegisterActionType(PostOpenAllocCallback::Create);
	actionPostCharInfoAllocCallback = __KernelRegisterActionType(PostCharInfoAllocCallback::Create);
	actionPostCharInfoFreeCallback = __KernelRegisterActionType(PostCharInfoFreeCallback::Create);
}

void __FontShutdown() {
	for (auto iter = fontMap.begin(); iter != fontMap.end(); iter++) {
		FontLib *fontLib = iter->second->GetFontLib();
		if (fontLib)
			fontLib->CloseFont(iter->second);
		delete iter->second;
	}
	fontMap.clear();
	for (auto iter = fontLibList.begin(); iter != fontLibList.end(); iter++) {
		delete *iter;
	}
	fontLibList.clear();
	fontLibMap.clear();
	for (auto iter = internalFonts.begin(); iter != internalFonts.end(); ++iter) {
		delete *iter;
	}
	internalFonts.clear();
}

void __FontDoState(PointerWrap &p) {
	auto s = p.Section("sceFont", 1, 2);
	if (!s)
		return;

	__LoadInternalFonts();

	Do(p, fontLibList);
	Do(p, fontLibMap);
	Do(p, fontMap);

	Do(p, actionPostAllocCallback);
	__KernelRestoreActionType(actionPostAllocCallback, PostAllocCallback::Create);
	Do(p, actionPostOpenCallback);
	__KernelRestoreActionType(actionPostOpenCallback, PostOpenCallback::Create);
	if (s >= 2) {
		Do(p, actionPostOpenAllocCallback);
		__KernelRestoreActionType(actionPostOpenAllocCallback, PostOpenAllocCallback::Create);
		Do(p, actionPostCharInfoAllocCallback);
		__KernelRestoreActionType(actionPostCharInfoAllocCallback, PostCharInfoAllocCallback::Create);
		Do(p, actionPostCharInfoFreeCallback);
		__KernelRestoreActionType(actionPostCharInfoFreeCallback, PostCharInfoFreeCallback::Create);
	} else {
		useAllocCallbacks = false;
	}
}

static u32 sceFontNewLib(u32 paramPtr, u32 errorCodePtr) {
	// Lazy load internal fonts, only when font library first inited.
	__LoadInternalFonts();

	auto params = PSPPointer<FontNewLibParams>::Create(paramPtr);
	auto errorCode = PSPPointer<u32>::Create(errorCodePtr);

	if (!params.IsValid() || !errorCode.IsValid()) {
		ERROR_LOG_REPORT(SCEFONT, "sceFontNewLib(%08x, %08x): invalid addresses", paramPtr, errorCodePtr);
		// The PSP would crash in this situation, not a real error code.
		return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	}
	if (!Memory::IsValidAddress(params->allocFuncAddr) || !Memory::IsValidAddress(params->freeFuncAddr)) {
		ERROR_LOG_REPORT(SCEFONT, "sceFontNewLib(%08x, %08x): missing alloc func", paramPtr, errorCodePtr);
		*errorCode = ERROR_FONT_INVALID_PARAMETER;
		return 0;
	}

	INFO_LOG(SCEFONT, "sceFontNewLib(%08x, %08x)", paramPtr, errorCodePtr);
	*errorCode = 0;

	FontLib *newLib = new FontLib(paramPtr, errorCodePtr);
	fontLibList.push_back(newLib);
	// The game should never see this value, the return value is replaced
	// by the action. Except if we disable the alloc, in this case we return
	// the handle correctly here.
	return hleDelayResult(newLib->handle(), "new fontlib", 30000);
}

static int sceFontDoneLib(u32 fontLibHandle) {
	INFO_LOG(SCEFONT, "sceFontDoneLib(%08x)", fontLibHandle);
	FontLib *fl = GetFontLib(fontLibHandle);
	if (fl) {
		fl->Done();
	}
	return 0;
}

// Open internal font into a FontLib
static u32 sceFontOpen(u32 libHandle, u32 index, u32 mode, u32 errorCodePtr) {
	auto errorCode = PSPPointer<int>::Create(errorCodePtr);
	if (!errorCode.IsValid()) {
		// Would crash on the PSP.
		ERROR_LOG(SCEFONT, "sceFontOpen(%x, %x, %x, %x): invalid pointer", libHandle, index, mode, errorCodePtr);
		return -1;
	}

	DEBUG_LOG(SCEFONT, "sceFontOpen(%x, %x, %x, %x)", libHandle, index, mode, errorCodePtr);
	FontLib *fontLib = GetFontLib(libHandle);
	if (fontLib == NULL) {
		*errorCode = ERROR_FONT_INVALID_LIBID;
		return 0;
	}
	if (index >= internalFonts.size()) {
		*errorCode = ERROR_FONT_INVALID_PARAMETER;
		return 0;
	}

	FontOpenMode openMode = mode == 0 ? FONT_OPEN_INTERNAL_STINGY : FONT_OPEN_INTERNAL_FULL;
	LoadedFont *font = fontLib->OpenFont(internalFonts[index], openMode, *errorCode);
	if (font) {
		*errorCode = 0;
		return hleDelayResult(font->Handle(), "font open", 10000);
	} else {
		return 0;
	}
}

// Open a user font in RAM into a FontLib
static u32 sceFontOpenUserMemory(u32 libHandle, u32 memoryFontAddrPtr, u32 memoryFontLength, u32 errorCodePtr) {
	auto errorCode = PSPPointer<int>::Create(errorCodePtr);
	if (!errorCode.IsValid()) {
		ERROR_LOG_REPORT(SCEFONT, "sceFontOpenUserMemory(%08x, %08x, %08x, %08x): invalid error address", libHandle, memoryFontAddrPtr, memoryFontLength, errorCodePtr);
		return -1;
	}
	if (!Memory::IsValidAddress(memoryFontAddrPtr)) {
		ERROR_LOG_REPORT(SCEFONT, "sceFontOpenUserMemory(%08x, %08x, %08x, %08x): invalid address", libHandle, memoryFontAddrPtr, memoryFontLength, errorCodePtr);
		*errorCode = ERROR_FONT_INVALID_PARAMETER;
		return 0;
	}

	FontLib *fontLib = GetFontLib(libHandle);
	if (!fontLib) {
		ERROR_LOG_REPORT(SCEFONT, "sceFontOpenUserMemory(%08x, %08x, %08x, %08x): bad font lib", libHandle, memoryFontAddrPtr, memoryFontLength, errorCodePtr);
		*errorCode = ERROR_FONT_INVALID_LIBID;
		return 0;
	}
	if (memoryFontLength == 0) {
		ERROR_LOG_REPORT(SCEFONT, "sceFontOpenUserMemory(%08x, %08x, %08x, %08x): invalid size", libHandle, memoryFontAddrPtr, memoryFontLength, errorCodePtr);
		*errorCode = ERROR_FONT_INVALID_PARAMETER;
		return 0;
	}

	DEBUG_LOG(SCEFONT, "sceFontOpenUserMemory(%08x, %08x, %08x, %08x)", libHandle, memoryFontAddrPtr, memoryFontLength, errorCodePtr);
	const u8 *fontData = Memory::GetPointer(memoryFontAddrPtr);
	// Games are able to overstate the size of a font.  Let's avoid crashing when we memcpy() it.
	// Unsigned 0xFFFFFFFF is treated as max, but that's impossible, so let's clamp to 64MB.
	if (memoryFontLength > 0x03FFFFFF)
		memoryFontLength = 0x03FFFFFF;
	while (!Memory::IsValidAddress(memoryFontAddrPtr + memoryFontLength - 1)) {
		--memoryFontLength;
	}
	Font *f = new Font(fontData, memoryFontLength);
	LoadedFont *font = fontLib->OpenFont(f, FONT_OPEN_USERBUFFER, *errorCode);
	if (font) {
		*errorCode = 0;
		return font->Handle();
	} else {
		delete f;
		return 0;
	}
}

// Open a user font in a file into a FontLib
static u32 sceFontOpenUserFile(u32 libHandle, const char *fileName, u32 mode, u32 errorCodePtr) {
	auto errorCode = PSPPointer<int>::Create(errorCodePtr);

	if (!errorCode.IsValid()) {
		ERROR_LOG_REPORT(SCEFONT, "sceFontOpenUserFile(%08x, %s, %08x, %08x): invalid error address", libHandle, fileName, mode, errorCodePtr);
		return ERROR_FONT_INVALID_PARAMETER;
	}

	if (fileName == NULL) {
		ERROR_LOG_REPORT(SCEFONT, "sceFontOpenUserFile(%08x, %s, %08x, %08x): invalid filename", libHandle, fileName, mode, errorCodePtr);
		*errorCode = ERROR_FONT_INVALID_PARAMETER;
		return 0;
	}

	FontLib *fontLib = GetFontLib(libHandle);
	if (!fontLib) {
		ERROR_LOG_REPORT(SCEFONT, "sceFontOpenUserFile(%08x, %s, %08x, %08x): invalid font lib", libHandle, fileName, mode, errorCodePtr);
		*errorCode = ERROR_FONT_INVALID_LIBID;
		return 0;
	}

	// TODO: Technically, we only do this if mode = 1.  Mode 0 uses the handlers.
	if (mode != 1) {
		WARN_LOG_REPORT(SCEFONT, "Loading file directly instead of using handlers: %s", fileName);
	}
	PSPFileInfo info = pspFileSystem.GetFileInfo(fileName);
	if (!info.exists) {
		ERROR_LOG_REPORT(SCEFONT, "sceFontOpenUserFile(%08x, %s, %08x, %08x): file does not exist", libHandle, fileName, mode, errorCodePtr);
		*errorCode = ERROR_FONT_HANDLER_OPEN_FAILED;
		return 0;
	}

	INFO_LOG(SCEFONT, "sceFontOpenUserFile(%08x, %s, %08x, %08x)", libHandle, fileName, mode, errorCodePtr);
	std::vector<u8> buffer;
	pspFileSystem.ReadEntireFile(fileName, buffer);
	Font *f = new Font(buffer);
	FontOpenMode openMode = mode == 0 ? FONT_OPEN_USERFILE_HANDLERS : FONT_OPEN_USERFILE_FULL;
	LoadedFont *font = fontLib->OpenFont(f, openMode, *errorCode);
	if (font) {
		*errorCode = 0;
		return font->Handle();
	} else {
		delete f;
		return 0;
	}
}

static int sceFontClose(u32 fontHandle) {
	LoadedFont *font = GetLoadedFont(fontHandle, false);
	if (font) {
		DEBUG_LOG(SCEFONT, "sceFontClose(%x)", fontHandle);
		FontLib *fontLib = font->GetFontLib();
		if (fontLib) {
			fontLib->CloseFont(font);
		}
	} else
		ERROR_LOG(SCEFONT, "sceFontClose(%x) - font not open?", fontHandle);
	return 0;
}

static int sceFontFindOptimumFont(u32 libHandle, u32 fontStylePtr, u32 errorCodePtr) {
	auto errorCode = PSPPointer<int>::Create(errorCodePtr);
	if (!errorCode.IsValid()) {
		ERROR_LOG_REPORT(SCEFONT, "sceFontFindOptimumFont(%08x, %08x, %08x): invalid error address", libHandle, fontStylePtr, errorCodePtr);
		return SCE_KERNEL_ERROR_INVALID_ARGUMENT;
	}

	FontLib *fontLib = GetFontLib(libHandle);
	if (!fontLib) {
		ERROR_LOG_REPORT(SCEFONT, "sceFontFindOptimumFont(%08x, %08x, %08x): invalid font lib", libHandle, fontStylePtr, errorCodePtr);
		*errorCode = ERROR_FONT_INVALID_LIBID;
		return 0;
	}

	if (!Memory::IsValidAddress(fontStylePtr)) {
		ERROR_LOG_REPORT(SCEFONT, "sceFontFindOptimumFont(%08x, %08x, %08x): invalid style address", libHandle, fontStylePtr, errorCodePtr);
		// Yes, actually.  Must've been a typo in the library.
		*errorCode = ERROR_FONT_INVALID_LIBID;
		return 0;
	}

	DEBUG_LOG(SCEFONT, "sceFontFindOptimumFont(%08x, %08x, %08x)", libHandle, fontStylePtr, errorCodePtr);

	auto requestedStyle = PSPPointer<const PGFFontStyle>::Create(fontStylePtr);

	// Find the first nearest match for H/V, OR the last exact match for others.
	float hRes = requestedStyle->fontHRes > 0.0f ? requestedStyle->fontHRes : fontLib->FontHRes();
	float vRes = requestedStyle->fontVRes > 0.0f ? requestedStyle->fontVRes : fontLib->FontVRes();
	Font *optimumFont = 0;
	Font *nearestFont = 0;
	float nearestDist = std::numeric_limits<float>::infinity();
	for (size_t i = 0; i < internalFonts.size(); i++) {
		MatchQuality q = internalFonts[i]->MatchesStyle(*requestedStyle);
		if (q != MATCH_NONE) {
			auto matchStyle = internalFonts[i]->GetFontStyle();
			if (requestedStyle->fontH > 0.0f) {
				float hDist = fabs(matchStyle.fontHRes * matchStyle.fontH - hRes * requestedStyle->fontH);
				if (hDist < nearestDist) {
					nearestDist = hDist;
					nearestFont = internalFonts[i];
				}
			}
			if (requestedStyle->fontV > 0.0f) {
				// Appears to be a bug?  It seems to match H instead of V.
				float vDist = fabs(matchStyle.fontVRes * matchStyle.fontV - vRes * requestedStyle->fontH);
				if (vDist < nearestDist) {
					nearestDist = vDist;
					nearestFont = internalFonts[i];
				}
			}
		}
		if (q == MATCH_GOOD) {
			optimumFont = internalFonts[i];
		}
	}
	if (nearestFont) {
		optimumFont = nearestFont;
	}
	if (optimumFont) {
		*errorCode = 0;
		return GetInternalFontIndex(optimumFont);
	} else {
		*errorCode = 0;
		return 0;
	}
}

// Returns the font index, not handle
static int sceFontFindFont(u32 libHandle, u32 fontStylePtr, u32 errorCodePtr) {
	auto errorCode = PSPPointer<int>::Create(errorCodePtr);
	if (!errorCode.IsValid()) {
		ERROR_LOG_REPORT(SCEFONT, "sceFontFindFont(%x, %x, %x): invalid error address", libHandle, fontStylePtr, errorCodePtr);
		return SCE_KERNEL_ERROR_INVALID_ARGUMENT;
	}

	FontLib *fontLib = GetFontLib(libHandle);
	if (!fontLib) {
		ERROR_LOG_REPORT(SCEFONT, "sceFontFindFont(%08x, %08x, %08x): invalid font lib", libHandle, fontStylePtr, errorCodePtr);
		*errorCode = ERROR_FONT_INVALID_LIBID;
		return 0;
	}

	if (!Memory::IsValidAddress(fontStylePtr)) {
		ERROR_LOG_REPORT(SCEFONT, "sceFontFindFont(%08x, %08x, %08x): invalid style address", libHandle, fontStylePtr, errorCodePtr);
		*errorCode = ERROR_FONT_INVALID_PARAMETER;
		return 0;
	}

	DEBUG_LOG(SCEFONT, "sceFontFindFont(%x, %x, %x)", libHandle, fontStylePtr, errorCodePtr);

	auto requestedStyle = PSPPointer<const PGFFontStyle>::Create(fontStylePtr);

	// Find the closest exact match for the fields specified.
	float hRes = requestedStyle->fontHRes > 0.0f ? requestedStyle->fontHRes : fontLib->FontHRes();
	float vRes = requestedStyle->fontVRes > 0.0f ? requestedStyle->fontVRes : fontLib->FontVRes();
	for (size_t i = 0; i < internalFonts.size(); i++) {
		if (internalFonts[i]->MatchesStyle(*requestedStyle) != MATCH_NONE) {
			auto matchStyle = internalFonts[i]->GetFontStyle();
			if (requestedStyle->fontH > 0.0f) {
				float hDist = fabs(matchStyle.fontHRes * matchStyle.fontH - hRes * requestedStyle->fontH);
				if (hDist > 0.001f) {
					continue;
				}
			} else if (requestedStyle->fontV > 0.0f) {
				// V seems to be ignored, unless H isn't specified.
				// If V is specified alone, the match always fails.
				continue;
			}
			*errorCode = 0;
			return (int)i;
		}
	}
	*errorCode = 0;
	return -1;
}

static int sceFontGetFontInfo(u32 fontHandle, u32 fontInfoPtr) {
	if (!Memory::IsValidAddress(fontInfoPtr)) {
		ERROR_LOG(SCEFONT, "sceFontGetFontInfo(%x, %x): bad fontInfo pointer", fontHandle, fontInfoPtr);
		return ERROR_FONT_INVALID_PARAMETER;
	}
	LoadedFont *font = GetLoadedFont(fontHandle, true);
	if (!font) {
		ERROR_LOG_REPORT(SCEFONT, "sceFontGetFontInfo(%x, %x): bad font", fontHandle, fontInfoPtr);
		return ERROR_FONT_INVALID_PARAMETER;
	}

	DEBUG_LOG(SCEFONT, "sceFontGetFontInfo(%x, %x)", fontHandle, fontInfoPtr);
	auto fi = PSPPointer<PGFFontInfo>::Create(fontInfoPtr);
	font->GetPGF()->GetFontInfo(fi);
	fi->fontStyle = font->GetFont()->GetFontStyle();

	return 0;
}

// It says FontInfo but it means Style - this is like sceFontGetFontList().
static int sceFontGetFontInfoByIndexNumber(u32 libHandle, u32 fontInfoPtr, u32 index) {
	auto fontStyle = PSPPointer<PGFFontStyle>::Create(fontInfoPtr);
	FontLib *fl = GetFontLib(libHandle);
	if (!fl || fl->handle() == 0) {
		ERROR_LOG_REPORT(SCEFONT, "sceFontGetFontInfoByIndexNumber(%08x, %08x, %i): invalid font lib", libHandle, fontInfoPtr, index);
		return !fl ? ERROR_FONT_INVALID_LIBID : ERROR_FONT_INVALID_PARAMETER;
	}
	if (index >= internalFonts.size()) {
		ERROR_LOG_REPORT(SCEFONT, "sceFontGetFontInfoByIndexNumber(%08x, %08x, %i): invalid font index", libHandle, fontInfoPtr, index);
		return ERROR_FONT_INVALID_PARAMETER;
	}
	if (!fontStyle.IsValid()) {
		ERROR_LOG_REPORT(SCEFONT, "sceFontGetFontInfoByIndexNumber(%08x, %08x, %i): invalid info pointer", libHandle, fontInfoPtr, index);
		return ERROR_FONT_INVALID_PARAMETER;
	}

	DEBUG_LOG(SCEFONT, "sceFontGetFontInfoByIndexNumber(%08x, %08x, %i)", libHandle, fontInfoPtr, index);
	auto font = internalFonts[index];
	*fontStyle = font->GetFontStyle();

	return 0;
}

static int sceFontGetCharInfo(u32 fontHandle, u32 charCode, u32 charInfoPtr) {
	charCode &= 0xffff;
	if (!Memory::IsValidAddress(charInfoPtr)) {
		ERROR_LOG(SCEFONT, "sceFontGetCharInfo(%08x, %i, %08x): bad charInfo pointer", fontHandle, charCode, charInfoPtr);
		return ERROR_FONT_INVALID_PARAMETER;
	}
	LoadedFont *font = GetLoadedFont(fontHandle, true);
	if (!font) {
		// The PSP crashes, but we assume it'd work like sceFontGetFontInfo(), and not touch charInfo.
		ERROR_LOG_REPORT(SCEFONT, "sceFontGetCharInfo(%08x, %i, %08x): bad font", fontHandle, charCode, charInfoPtr);
		return ERROR_FONT_INVALID_PARAMETER;
	}

	DEBUG_LOG(SCEFONT, "sceFontGetCharInfo(%08x, %i, %08x)", fontHandle, charCode, charInfoPtr);
	auto charInfo = PSPPointer<PGFCharInfo>::Create(charInfoPtr);
	font->GetCharInfo(charCode, charInfo);

	if (!useAllocCallbacks)
		return 0;

	u32 allocSize = charInfo->bitmapWidth * charInfo->bitmapHeight;
	if (charInfo->sfp26AdvanceH != 0 || charInfo->sfp26AdvanceV != 0) {
		if (font->GetFontLib()->GetCharInfoBitmapAddress() != 0) {
			PostCharInfoFreeCallback *action = (PostCharInfoFreeCallback *)__KernelCreateAction(actionPostCharInfoFreeCallback);
			action->SetFontLib(font->GetFontLib()->GetListID());
			action->SetCharInfo(charInfo);

			u32 args[2] = { font->GetFontLib()->userDataAddr(), font->GetFontLib()->GetCharInfoBitmapAddress() };
			hleEnqueueCall(font->GetFontLib()->freeFuncAddr(), 2, args, action);
		} else {
			PostCharInfoAllocCallback *action = (PostCharInfoAllocCallback *)__KernelCreateAction(actionPostCharInfoAllocCallback);
			action->SetFontLib(font->GetFontLib()->GetListID());

			u32 args[2] = { font->GetFontLib()->userDataAddr(), allocSize };
			hleEnqueueCall(font->GetFontLib()->allocFuncAddr(), 2, args, action);
		}
	}

	return 0;
}

static int sceFontGetShadowInfo(u32 fontHandle, u32 charCode, u32 charInfoPtr) {
	charCode &= 0xffff;
	if (!Memory::IsValidAddress(charInfoPtr)) {
		ERROR_LOG(SCEFONT, "sceFontGetShadowInfo(%08x, %i, %08x): bad charInfo pointer", fontHandle, charCode, charInfoPtr);
		return ERROR_FONT_INVALID_PARAMETER;
	}
	LoadedFont *font = GetLoadedFont(fontHandle, true);
	if (!font) {
		ERROR_LOG_REPORT(SCEFONT, "sceFontGetShadowInfo(%08x, %i, %08x): bad font", fontHandle, charCode, charInfoPtr);
		return ERROR_FONT_INVALID_PARAMETER;
	}

	DEBUG_LOG(SCEFONT, "sceFontGetShadowInfo(%08x, %i, %08x)", fontHandle, charCode, charInfoPtr);
	auto charInfo = PSPPointer<PGFCharInfo>::Create(charInfoPtr);
	font->GetCharInfo(charCode, charInfo, FONT_PGF_SHADOWGLYPH);

	return 0;
}

static int sceFontGetCharImageRect(u32 fontHandle, u32 charCode, u32 charRectPtr) {
	charCode &= 0xffff;
	auto charRect = PSPPointer<FontImageRect>::Create(charRectPtr);
	LoadedFont *font = GetLoadedFont(fontHandle, true);
	if (!font) {
		ERROR_LOG_REPORT(SCEFONT, "sceFontGetCharImageRect(%08x, %i, %08x): bad font", fontHandle, charCode, charRectPtr);
		return ERROR_FONT_INVALID_PARAMETER;
	}
	if (!charRect.IsValid()) {
		ERROR_LOG_REPORT(SCEFONT, "sceFontGetCharImageRect(%08x, %i, %08x): invalid rect pointer", fontHandle, charCode, charRectPtr);
		return ERROR_FONT_INVALID_PARAMETER;
	}

	DEBUG_LOG(SCEFONT, "sceFontGetCharImageRect(%08x, %i, %08x)", fontHandle, charCode, charRectPtr);
	PGFCharInfo charInfo;
	font->GetCharInfo(charCode, &charInfo);
	charRect->width = charInfo.bitmapWidth;
	charRect->height = charInfo.bitmapHeight;
	return 0;
}

static int sceFontGetShadowImageRect(u32 fontHandle, u32 charCode, u32 charRectPtr) {
	charCode &= 0xffff;
	auto charRect = PSPPointer<FontImageRect>::Create(charRectPtr);
	LoadedFont *font = GetLoadedFont(fontHandle, true);
	if (!font) {
		ERROR_LOG_REPORT(SCEFONT, "sceFontGetShadowImageRect(%08x, %i, %08x): bad font", fontHandle, charCode, charRectPtr);
		return ERROR_FONT_INVALID_PARAMETER;
	}
	if (!charRect.IsValid()) {
		ERROR_LOG_REPORT(SCEFONT, "sceFontGetShadowImageRect(%08x, %i, %08x): invalid rect pointer", fontHandle, charCode, charRectPtr);
		return ERROR_FONT_INVALID_PARAMETER;
	}

	DEBUG_LOG(SCEFONT, "sceFontGetShadowImageRect(%08x, %i, %08x)", fontHandle, charCode, charRectPtr);
	PGFCharInfo charInfo;
	font->GetCharInfo(charCode, &charInfo, FONT_PGF_SHADOWGLYPH);
	charRect->width = charInfo.bitmapWidth;
	charRect->height = charInfo.bitmapHeight;
	return 0;
}

static int sceFontGetCharGlyphImage(u32 fontHandle, u32 charCode, u32 glyphImagePtr) {
	charCode &= 0xffff;
	if (!Memory::IsValidAddress(glyphImagePtr)) {
		ERROR_LOG(SCEFONT, "sceFontGetCharGlyphImage(%x, %x, %x): bad glyphImage pointer", fontHandle, charCode, glyphImagePtr);
		return ERROR_FONT_INVALID_PARAMETER;
	}
	LoadedFont *font = GetLoadedFont(fontHandle, true);
	if (!font) {
		ERROR_LOG_REPORT(SCEFONT, "sceFontGetCharGlyphImage(%x, %x, %x): bad font", fontHandle, charCode, glyphImagePtr);
		return ERROR_FONT_INVALID_PARAMETER;
	}

	DEBUG_LOG(SCEFONT, "sceFontGetCharGlyphImage(%x, %x, %x)", fontHandle, charCode, glyphImagePtr);
	auto glyph = PSPPointer<const GlyphImage>::Create(glyphImagePtr);
	font->DrawCharacter(glyph, -1, -1, -1, -1, charCode, FONT_PGF_CHARGLYPH);
	return 0;
}

static int sceFontGetCharGlyphImage_Clip(u32 fontHandle, u32 charCode, u32 glyphImagePtr, int clipXPos, int clipYPos, int clipWidth, int clipHeight) {
	charCode &= 0xffff;
	if (!Memory::IsValidAddress(glyphImagePtr)) {
		ERROR_LOG(SCEFONT, "sceFontGetCharGlyphImage_Clip(%08x, %i, %08x, %i, %i, %i, %i): bad glyphImage pointer", fontHandle, charCode, glyphImagePtr, clipXPos, clipYPos, clipWidth, clipHeight);
		return ERROR_FONT_INVALID_PARAMETER;
	}
	LoadedFont *font = GetLoadedFont(fontHandle, true);
	if (!font) {
		ERROR_LOG_REPORT(SCEFONT, "sceFontGetCharGlyphImage_Clip(%08x, %i, %08x, %i, %i, %i, %i): bad font", fontHandle, charCode, glyphImagePtr, clipXPos, clipYPos, clipWidth, clipHeight);
		return ERROR_FONT_INVALID_PARAMETER;
	}

	DEBUG_LOG(SCEFONT, "sceFontGetCharGlyphImage_Clip(%08x, %i, %08x, %i, %i, %i, %i)", fontHandle, charCode, glyphImagePtr, clipXPos, clipYPos, clipWidth, clipHeight);
	auto glyph = PSPPointer<const GlyphImage>::Create(glyphImagePtr);
	font->DrawCharacter(glyph, clipXPos, clipYPos, clipWidth, clipHeight, charCode, FONT_PGF_CHARGLYPH);
	return 0;
}

static int sceFontSetAltCharacterCode(u32 fontLibHandle, u32 charCode) {
	charCode &= 0xffff;
	FontLib *fl = GetFontLib(fontLibHandle);
	if (!fl) {
		ERROR_LOG_REPORT(SCEFONT, "sceFontSetAltCharacterCode(%08x, %08x): invalid font lib", fontLibHandle, charCode);
		return ERROR_FONT_INVALID_LIBID;
	}

	INFO_LOG(SCEFONT, "sceFontSetAltCharacterCode(%08x, %08x)", fontLibHandle, charCode);
	fl->SetAltCharCode(charCode & 0xFFFF);
	return 0;
}

static int sceFontFlush(u32 fontHandle) {
	INFO_LOG(SCEFONT, "sceFontFlush(%i)", fontHandle);
	
	LoadedFont *font = GetLoadedFont(fontHandle, true);
	if (!font) {
		ERROR_LOG_REPORT(SCEFONT, "sceFontFlush(%08x): bad font", fontHandle);
		return ERROR_FONT_INVALID_PARAMETER;
	}

	font->GetFontLib()->flushFont();

	return 0;
}

// One would think that this should loop through the fonts loaded in the fontLibHandle,
// but it seems not.
static int sceFontGetFontList(u32 fontLibHandle, u32 fontStylePtr, int numFonts) {
	auto fontStyles = PSPPointer<PGFFontStyle>::Create(fontStylePtr);
	FontLib *fl = GetFontLib(fontLibHandle);
	if (!fl) {
		ERROR_LOG_REPORT(SCEFONT, "sceFontGetFontList(%08x, %08x, %i): invalid font lib", fontLibHandle, fontStylePtr, numFonts);
		return ERROR_FONT_INVALID_LIBID;
	}
	if (!fontStyles.IsValid()) {
		ERROR_LOG_REPORT(SCEFONT, "sceFontGetFontList(%08x, %08x, %i): invalid style pointer", fontLibHandle, fontStylePtr, numFonts);
		return ERROR_FONT_INVALID_PARAMETER;
	}

	DEBUG_LOG(SCEFONT, "sceFontGetFontList(%08x, %08x, %i)", fontLibHandle, fontStylePtr, numFonts);
	if (fl->handle() != 0) {
		numFonts = std::min(numFonts, (int)internalFonts.size());
		for (int i = 0; i < numFonts; i++)
			fontStyles[i] = internalFonts[i]->GetFontStyle();
	}

	return hleDelayResult(0, "font list read", 100);
}

static int sceFontGetNumFontList(u32 fontLibHandle, u32 errorCodePtr) {
	auto errorCode = PSPPointer<int>::Create(errorCodePtr);
	if (!errorCode.IsValid()) {
		ERROR_LOG_REPORT(SCEFONT, "sceFontGetNumFontList(%08x, %08x): invalid error address", fontLibHandle, errorCodePtr);
		return ERROR_FONT_INVALID_PARAMETER;
	}
	FontLib *fl = GetFontLib(fontLibHandle);
	if (!fl) {
		ERROR_LOG_REPORT(SCEFONT, "sceFontGetNumFontList(%08x, %08x): invalid font lib", fontLibHandle, errorCodePtr);
		*errorCode = ERROR_FONT_INVALID_LIBID;
		return 0;
	}
	DEBUG_LOG(SCEFONT, "sceFontGetNumFontList(%08x, %08x)", fontLibHandle, errorCodePtr);
	*errorCode = 0;
	return fl->handle() == 0 ? 0 : (int)internalFonts.size();
}

static int sceFontSetResolution(u32 fontLibHandle, float hRes, float vRes) {
	FontLib *fl = GetFontLib(fontLibHandle);
	if (!fl) {
		ERROR_LOG_REPORT(SCEFONT, "sceFontSetResolution(%08x, %f, %f): invalid font lib", fontLibHandle, hRes, vRes);
		return ERROR_FONT_INVALID_LIBID;
	}
	if (hRes <= 0.0f || vRes <= 0.0f) {
		ERROR_LOG_REPORT(SCEFONT, "sceFontSetResolution(%08x, %f, %f): negative value", fontLibHandle, hRes, vRes);
		return ERROR_FONT_INVALID_PARAMETER;
	}
	INFO_LOG(SCEFONT, "sceFontSetResolution(%08x, %f, %f)", fontLibHandle, hRes, vRes);
	fl->SetResolution(hRes, vRes);
	return 0;
}

static float sceFontPixelToPointH(int fontLibHandle, float fontPixelsH, u32 errorCodePtr) {
	auto errorCode = PSPPointer<int>::Create(errorCodePtr);
	if (!errorCode.IsValid()) {
		ERROR_LOG_REPORT(SCEFONT, "sceFontPixelToPointH(%08x, %f, %08x): invalid error address", fontLibHandle, fontPixelsH, errorCodePtr);
		return 0.0f;
	}
	FontLib *fl = GetFontLib(fontLibHandle);
	if (!fl) {
		ERROR_LOG_REPORT(SCEFONT, "sceFontPixelToPointH(%08x, %f, %08x): invalid font lib", fontLibHandle, fontPixelsH, errorCodePtr);
		*errorCode = ERROR_FONT_INVALID_LIBID;
		return 0.0f;
	}
	DEBUG_LOG(SCEFONT, "sceFontPixelToPointH(%08x, %f, %08x)", fontLibHandle, fontPixelsH, errorCodePtr);
	*errorCode = 0;
	return fontPixelsH * pointDPI / fl->FontHRes();
}

static float sceFontPixelToPointV(int fontLibHandle, float fontPixelsV, u32 errorCodePtr) {
	auto errorCode = PSPPointer<int>::Create(errorCodePtr);
	if (!errorCode.IsValid()) {
		ERROR_LOG_REPORT(SCEFONT, "sceFontPixelToPointV(%08x, %f, %08x): invalid error address", fontLibHandle, fontPixelsV, errorCodePtr);
		return 0.0f;
	}
	FontLib *fl = GetFontLib(fontLibHandle);
	if (!fl) {
		ERROR_LOG_REPORT(SCEFONT, "sceFontPixelToPointV(%08x, %f, %08x): invalid font lib", fontLibHandle, fontPixelsV, errorCodePtr);
		*errorCode = ERROR_FONT_INVALID_LIBID;
		return 0.0f;
	}
	DEBUG_LOG(SCEFONT, "sceFontPixelToPointV(%08x, %f, %08x)", fontLibHandle, fontPixelsV, errorCodePtr);
	*errorCode = 0;
	return fontPixelsV * pointDPI / fl->FontVRes();
}

static float sceFontPointToPixelH(int fontLibHandle, float fontPointsH, u32 errorCodePtr) {
	auto errorCode = PSPPointer<int>::Create(errorCodePtr);
	if (!errorCode.IsValid()) {
		ERROR_LOG_REPORT(SCEFONT, "sceFontPointToPixelH(%08x, %f, %08x): invalid error address", fontLibHandle, fontPointsH, errorCodePtr);
		return 0.0f;
	}
	FontLib *fl = GetFontLib(fontLibHandle);
	if (!fl) {
		ERROR_LOG_REPORT(SCEFONT, "sceFontPointToPixelH(%08x, %f, %08x): invalid font lib", fontLibHandle, fontPointsH, errorCodePtr);
		*errorCode = ERROR_FONT_INVALID_LIBID;
		return 0.0f;
	}
	DEBUG_LOG(SCEFONT, "sceFontPointToPixelH(%08x, %f, %08x)", fontLibHandle, fontPointsH, errorCodePtr);
	*errorCode = 0;
	return fontPointsH * fl->FontHRes() / pointDPI;
}

static float sceFontPointToPixelV(int fontLibHandle, float fontPointsV, u32 errorCodePtr) {
	auto errorCode = PSPPointer<int>::Create(errorCodePtr);
	if (!errorCode.IsValid()) {
		ERROR_LOG_REPORT(SCEFONT, "sceFontPointToPixelV(%08x, %f, %08x): invalid error address", fontLibHandle, fontPointsV, errorCodePtr);
		return 0.0f;
	}
	FontLib *fl = GetFontLib(fontLibHandle);
	if (!fl) {
		ERROR_LOG_REPORT(SCEFONT, "sceFontPointToPixelV(%08x, %f, %08x): invalid font lib", fontLibHandle, fontPointsV, errorCodePtr);
		*errorCode = ERROR_FONT_INVALID_LIBID;
		return 0.0f;
	}
	DEBUG_LOG(SCEFONT, "sceFontPointToPixelV(%08x, %f, %08x)", fontLibHandle, fontPointsV, errorCodePtr);
	*errorCode = 0;
	return fontPointsV * fl->FontVRes() / pointDPI;
}

static int sceFontCalcMemorySize() {
	ERROR_LOG_REPORT(SCEFONT, "UNIMPL sceFontCalcMemorySize()");
	return 0;
}

static int sceFontGetShadowGlyphImage(u32 fontHandle, u32 charCode, u32 glyphImagePtr) {
	charCode &= 0xffff;
	if (!Memory::IsValidAddress(glyphImagePtr)) {
		ERROR_LOG(SCEFONT, "sceFontGetShadowGlyphImage(%x, %x, %x): bad glyphImage pointer", fontHandle, charCode, glyphImagePtr);
		return ERROR_FONT_INVALID_PARAMETER;
	}
	LoadedFont *font = GetLoadedFont(fontHandle, true);
	if (!font) {
		ERROR_LOG_REPORT(SCEFONT, "sceFontGetShadowGlyphImage(%x, %x, %x): bad font", fontHandle, charCode, glyphImagePtr);
		return ERROR_FONT_INVALID_PARAMETER;
	}

	DEBUG_LOG(SCEFONT, "sceFontGetShadowGlyphImage(%x, %x, %x)", fontHandle, charCode, glyphImagePtr);
	auto glyph = PSPPointer<const GlyphImage>::Create(glyphImagePtr);
	font->DrawCharacter(glyph, -1, -1, -1, -1, charCode, FONT_PGF_SHADOWGLYPH);
	return 0;
}

static int sceFontGetShadowGlyphImage_Clip(u32 fontHandle, u32 charCode, u32 glyphImagePtr, int clipXPos, int clipYPos, int clipWidth, int clipHeight) {
	charCode &= 0xffff;
	if (!Memory::IsValidAddress(glyphImagePtr)) {
		ERROR_LOG(SCEFONT, "sceFontGetShadowGlyphImage_Clip(%08x, %i, %08x, %i, %i, %i, %i): bad glyphImage pointer", fontHandle, charCode, glyphImagePtr, clipXPos, clipYPos, clipWidth, clipHeight);
		return ERROR_FONT_INVALID_PARAMETER;
	}
	LoadedFont *font = GetLoadedFont(fontHandle, true);
	if (!font) {
		ERROR_LOG_REPORT(SCEFONT, "sceFontGetShadowGlyphImage_Clip(%08x, %i, %08x, %i, %i, %i, %i): bad font", fontHandle, charCode, glyphImagePtr, clipXPos, clipYPos, clipWidth, clipHeight);
		return ERROR_FONT_INVALID_PARAMETER;
	}

	DEBUG_LOG(SCEFONT, "sceFontGetShadowGlyphImage_Clip(%08x, %i, %08x, %i, %i, %i, %i)", fontHandle, charCode, glyphImagePtr, clipXPos, clipYPos, clipWidth, clipHeight);
	auto glyph = PSPPointer<const GlyphImage>::Create(glyphImagePtr);
	font->DrawCharacter(glyph, clipXPos, clipYPos, clipWidth, clipHeight, charCode, FONT_PGF_SHADOWGLYPH);
	return 0;
}

// sceLibFont is a user level library so it can touch the stack. Some games appear to rely a bit of stack
// being wiped - although in reality, it won't be wiped with just zeroes..
const HLEFunction sceLibFont[] = {
	{0X67F17ED7, &WrapU_UU<sceFontNewLib>,                        "sceFontNewLib",                   'x', "xx",      HLE_CLEAR_STACK_BYTES, 0x5A0 },
	{0X574B6FBC, &WrapI_U<sceFontDoneLib>,                        "sceFontDoneLib",                  'i', "x",       HLE_CLEAR_STACK_BYTES, 0x2C  },
	{0X48293280, &WrapI_UFF<sceFontSetResolution>,                "sceFontSetResolution",            'i', "xff"      },
	{0X27F6E642, &WrapI_UU<sceFontGetNumFontList>,                "sceFontGetNumFontList",           'i', "xx"       },
	{0XBC75D85B, &WrapI_UUI<sceFontGetFontList>,                  "sceFontGetFontList",              'i', "xxi",     HLE_CLEAR_STACK_BYTES, 0x31C },
	{0X099EF33C, &WrapI_UUU<sceFontFindOptimumFont>,              "sceFontFindOptimumFont",          'i', "xxx",     HLE_CLEAR_STACK_BYTES, 0xF0  },
	{0X681E61A7, &WrapI_UUU<sceFontFindFont>,                     "sceFontFindFont",                 'i', "xxx",     HLE_CLEAR_STACK_BYTES, 0x40  },
	{0X2F67356A, &WrapI_V<sceFontCalcMemorySize>,                 "sceFontCalcMemorySize",           'i', ""         },
	{0X5333322D, &WrapI_UUU<sceFontGetFontInfoByIndexNumber>,     "sceFontGetFontInfoByIndexNumber", 'i', "xxx",     HLE_CLEAR_STACK_BYTES, 0x20  },
	{0XA834319D, &WrapU_UUUU<sceFontOpen>,                        "sceFontOpen",                     'x', "xxxx",    HLE_CLEAR_STACK_BYTES, 0x460 },
	{0X57FCB733, &WrapU_UCUU<sceFontOpenUserFile>,                "sceFontOpenUserFile",             'x', "xsxx"     },
	{0XBB8E7FE6, &WrapU_UUUU<sceFontOpenUserMemory>,              "sceFontOpenUserMemory",           'x', "xxxx",    HLE_CLEAR_STACK_BYTES, 0x440 /*from JPCSP*/ },
	{0X3AEA8CB6, &WrapI_U<sceFontClose>,                          "sceFontClose",                    'i', "x",       HLE_CLEAR_STACK_BYTES, 0x54  },
	{0X0DA7535E, &WrapI_UU<sceFontGetFontInfo>,                   "sceFontGetFontInfo",              'i', "xx"       },
	{0XDCC80C2F, &WrapI_UUU<sceFontGetCharInfo>,                  "sceFontGetCharInfo",              'i', "xxx",     HLE_CLEAR_STACK_BYTES, 0x110 },
	{0XAA3DE7B5, &WrapI_UUU<sceFontGetShadowInfo>,                "sceFontGetShadowInfo",            'i', "xxx",     HLE_CLEAR_STACK_BYTES, 0x150 },
	{0X5C3E4A9E, &WrapI_UUU<sceFontGetCharImageRect>,             "sceFontGetCharImageRect",         'i', "xxx",     HLE_CLEAR_STACK_BYTES, 0x120 },
	{0X48B06520, &WrapI_UUU<sceFontGetShadowImageRect>,           "sceFontGetShadowImageRect",       'i', "xxx",     HLE_CLEAR_STACK_BYTES, 0x150 },
	{0X980F4895, &WrapI_UUU<sceFontGetCharGlyphImage>,            "sceFontGetCharGlyphImage",        'i', "xxx",     HLE_CLEAR_STACK_BYTES, 0x120 },
	{0XCA1E6945, &WrapI_UUUIIII<sceFontGetCharGlyphImage_Clip>,   "sceFontGetCharGlyphImage_Clip",   'i', "xxxiiii", HLE_CLEAR_STACK_BYTES, 0x130 },
	{0X74B21701, &WrapF_IFU<sceFontPixelToPointH>,                "sceFontPixelToPointH",            'f', "ifx",     HLE_CLEAR_STACK_BYTES, 0x10  },
	{0XF8F0752E, &WrapF_IFU<sceFontPixelToPointV>,                "sceFontPixelToPointV",            'f', "ifx",     HLE_CLEAR_STACK_BYTES, 0x10  },
	{0X472694CD, &WrapF_IFU<sceFontPointToPixelH>,                "sceFontPointToPixelH",            'f', "ifx"      },
	{0X3C4B7E82, &WrapF_IFU<sceFontPointToPixelV>,                "sceFontPointToPixelV",            'f', "ifx"      },
	{0XEE232411, &WrapI_UU<sceFontSetAltCharacterCode>,           "sceFontSetAltCharacterCode",      'i', "xx"       },
	{0X568BE516, &WrapI_UUU<sceFontGetShadowGlyphImage>,          "sceFontGetShadowGlyphImage",      'i', "xxx",     HLE_CLEAR_STACK_BYTES, 0x160 },
	{0X5DCF6858, &WrapI_UUUIIII<sceFontGetShadowGlyphImage_Clip>, "sceFontGetShadowGlyphImage_Clip", 'i', "xxxiiii", HLE_CLEAR_STACK_BYTES, 0x170 },
	{0X02D7F94B, &WrapI_U<sceFontFlush>,                          "sceFontFlush",                    'i', "x"        },
};

void Register_sceFont() {
	RegisterModule("sceLibFont", ARRAY_SIZE(sceLibFont), sceLibFont);
}

void Register_sceLibFttt() {
	RegisterModule("sceLibFttt", ARRAY_SIZE(sceLibFont), sceLibFont);
}
