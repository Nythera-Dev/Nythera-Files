#pragma once

#include <cstddef>

namespace filemanager::menu_id {

  inline constexpr int kCopy = 1;
  inline constexpr int kCut = 2;
  inline constexpr int kPaste = 3;
  inline constexpr int kRename = 4;
  inline constexpr int kTrash = 5;
  inline constexpr int kDeletePermanently = 6;
  inline constexpr int kOpen = 10;
  inline constexpr int kRefresh = 13;
  inline constexpr int kToggleViewMode = 14;
  inline constexpr int kPinTargetToPlaces = 20;
  inline constexpr int kPinCurrentToPlaces = 21;
  inline constexpr int kProperties = 22;

  inline constexpr int kPlaceOpen = 30;
  inline constexpr int kPlaceRemove = 31;
  inline constexpr int kPlacePin = 32;
  inline constexpr int kPlaceMount = 33;
  inline constexpr int kPlaceUnmount = 34;
  inline constexpr int kPlaceEject = 35;
  inline constexpr int kPlaceHide = 36;
  inline constexpr int kPlaceUnhide = 37;
  inline constexpr int kPlaceToggleShowHidden = 38;
  inline constexpr int kPlaceProperties = 39;
  inline constexpr int kPlaceOpenNewWindow = 40;
  inline constexpr int kPlaceOpenInNewTab = 41;

  inline constexpr int kShowHidden = 50;
  inline constexpr int kFoldersFirst = 51;
  inline constexpr int kHiddenLast = 52;
  inline constexpr int kSortName = 53;
  inline constexpr int kSortSize = 54;
  inline constexpr int kSortModified = 55;
  inline constexpr int kSortCreated = 56;
  inline constexpr int kSortAccessed = 57;
  inline constexpr int kSortAscending = 58;
  inline constexpr int kCopyLocation = 59;
  inline constexpr int kOpenFolderNewWindow = 60;
  inline constexpr int kSortType = 61;
  inline constexpr int kSortDescending = 62;
  inline constexpr int kSortLocation = 63;

  inline constexpr int kRestore = 70;
  inline constexpr int kRestoreTo = 71;
  inline constexpr int kEmptyWastebin = 72;

  inline constexpr int kOpenWithSubmenu = 80;
  inline constexpr int kOpenWithOther = 81;

  inline constexpr int kLocationCut = 90;
  inline constexpr int kLocationCopy = 91;
  inline constexpr int kLocationPaste = 92;
  inline constexpr int kLocationSelectAll = 93;

  inline constexpr int kSortBySubmenu = 100;

  inline constexpr int kCreateNewSubmenu = 110;
  inline constexpr int kCreateFolder = 111;
  inline constexpr int kCreateEmptyFile = 112;
  inline constexpr int kCreateTextFile = 113;
  inline constexpr int kCreateDocument = 114;
  inline constexpr int kCreateSpreadsheet = 115;
  inline constexpr int kCreatePresentation = 116;

  inline constexpr int kManualSymlink = 117;
  inline constexpr int kSelectionSymlink = 118;
  inline constexpr int kDuplicate = 119;
  inline constexpr int kSelectAll = 120;
  inline constexpr int kDeselectAll = 121;
  inline constexpr int kInvertSelection = 122;
  inline constexpr int kUndo = 123;
  inline constexpr int kRedo = 124;
  inline constexpr int kCompactEditMenu = 125;
  inline constexpr int kCompactViewMenu = 126;
  inline constexpr int kListView = 127;
  inline constexpr int kGridView = 128;
  inline constexpr int kShowColumns = 129;
  inline constexpr int kZoomIn = 130;
  inline constexpr int kZoomOut = 131;
  inline constexpr int kResetZoom = 132;
  inline constexpr int kFirstColumnAction = 133;

  inline constexpr int kFirstOpenWithApp = 20000;

  inline constexpr int kFileNewWindow = 30001;
  inline constexpr int kFileOpen = 30002;
  inline constexpr int kFileOpenWith = 30003;
  inline constexpr int kFileOpenNewWindow = 30004;
  inline constexpr int kFilePinToPlaces = 30005;
  inline constexpr int kFileProperties = 30006;
  inline constexpr int kQuit = 30007;
  inline constexpr int kShowMenuBar = 30008;
  inline constexpr int kPreferences = 30009;

  inline constexpr int kNewTab = 30010;
  inline constexpr int kReopenClosedTab = 30011;
  inline constexpr int kCloseTab = 30012;
  inline constexpr int kOpenFolderInNewTab = 30013;
  inline constexpr int kAlwaysShowTabBar = 30014;
  inline constexpr int kOpenTabsInBackground = 30015;
  inline constexpr int kFileOpenInNewTab = 30016;
  inline constexpr int kOpenContainingFolder = 30017;
  inline constexpr int kTabContextNew = 30020;
  inline constexpr int kTabContextDuplicate = 30021;
  inline constexpr int kTabContextClose = 30022;
  inline constexpr int kTabContextCloseOthers = 30023;
  inline constexpr int kTabContextCloseRight = 30024;
  inline constexpr int kFirstSearchMode = 30030;
  inline constexpr int kFirstSearchScope = 30040;
  inline constexpr int kSearchIncludeSystem = 30046;
  inline constexpr int kUseCustomSearch = 30047;
  inline constexpr int kDefineCustomSearch = 30048;
  inline constexpr int kManageCustomSearches = 30049;
  inline constexpr int kFirstFitColumn = 30050;
  inline constexpr int kFitAllColumns = 30056;
  inline constexpr int kSearchShowHidden = 30057;
  inline constexpr int kSearchFollowLinks = 30058;

  inline constexpr int kCompactToolsMenu = 30100;
  inline constexpr int kFirstToolMenuRow = 30101;
  inline constexpr int kCalculateChecksum = 30104;
  inline constexpr int kVerifyChecksum = 30105;
  inline constexpr int kActAsAdministrator = 30150;
  inline constexpr int kOpenFolderAsAdministrator = 30151;
  inline constexpr int kSaveProtectedChanges = 30152;

  inline constexpr int kCompactHelpMenu = 30200;
  inline constexpr int kKeyboardShortcuts = 30201;
  inline constexpr int kReportBug = 30202;
  inline constexpr int kGitHub = 30203;
  inline constexpr int kAboutFiles = 30204;

  inline constexpr int kFirstSearchFilterChoice = 30300;
  inline constexpr int kSearchFilterChoiceStride = 10;

  inline constexpr int kQuickLook = 30400;
  inline constexpr int kPreviewMode = 30401;
  inline constexpr int kPreviewHidden = 30402;
  inline constexpr int kPreviewInformation = 30403;
  inline constexpr int kPreviewFull = 30404;
  inline constexpr int kThumbnailSettings = 30405;
  inline constexpr int kThumbnailsNever = 30406;
  inline constexpr int kThumbnailsLocal = 30407;
  inline constexpr int kThumbnailsAll = 30408;
  inline constexpr int kInspectThumbnailCache = 30409;
  inline constexpr int kClearThumbnailCache = 30410;
  inline constexpr int kGoToLinkTarget = 30411;

  inline constexpr int kArchiveCompress = 30420;
  inline constexpr int kArchiveExtract = 30421;
  inline constexpr int kCompressTarGzip = 30422;
  inline constexpr int kCompressZip = 30423;
  inline constexpr int kCompressTo = 30424;
  inline constexpr int kExtractHere = 30425;
  inline constexpr int kExtractFolder = 30426;
  inline constexpr int kExtractTo = 30427;
  inline constexpr int kAvoidRedundantExtractionFolders = 30428;
  inline constexpr int kExtractSelected = 30429;
  inline constexpr int kOpenArchiveMemberWith = 30430;

  inline constexpr int kFirstExternalToolApp = 40000;

  inline constexpr std::size_t kToolsMenuIndex = 3;
  inline constexpr std::size_t kHelpMenuIndex = 4;

} // namespace filemanager::menu_id
