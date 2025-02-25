/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

let Cc = Components.classes;
let Ci = Components.interfaces;
let Cu = Components.utils;

Cu.import("resource://gre/modules/XPCOMUtils.jsm");
Cu.import("resource://gre/modules/FileUtils.jsm");
Cu.import("resource://gre/modules/Services.jsm");
Cu.import("resource:///modules/MigrationUtils.jsm");

XPCOMUtils.defineLazyModuleGetter(this, "PropertyListUtils",
                                  "resource://gre/modules/PropertyListUtils.jsm");
XPCOMUtils.defineLazyModuleGetter(this, "PlacesUtils",
                                  "resource://gre/modules/PlacesUtils.jsm");
XPCOMUtils.defineLazyModuleGetter(this, "NetUtil",
                                  "resource://gre/modules/NetUtil.jsm");
XPCOMUtils.defineLazyModuleGetter(this, "FormHistory",
                                  "resource://gre/modules/FormHistory.jsm");

function Bookmarks(aBookmarksFile) {
  this._file = aBookmarksFile;
}
Bookmarks.prototype = {
  type: MigrationUtils.resourceTypes.BOOKMARKS,

  migrate: function B_migrate(aCallback) {
    PropertyListUtils.read(this._file,
      MigrationUtils.wrapMigrateFunction(function migrateBookmarks(aDict) {
        if (!aDict)
          throw new Error("Could not read Bookmarks.plist");

        let children = aDict.get("Children");;
        if (!children)
          throw new Error("Invalid Bookmarks.plist format");

        PlacesUtils.bookmarks.runInBatchMode({
          runBatched: function() {
            let collection = aDict.get("Title") == "com.apple.ReadingList" ?
              this.READING_LIST_COLLECTION : this.ROOT_COLLECTION;
            this._migrateCollection(children, collection);
          }.bind(this)
        }, null);
      }.bind(this), aCallback));
  },

  // Bookmarks collections in Safari.  Constants for migrateCollection.
  ROOT_COLLECTION:         0,
  MENU_COLLECTION:         1,
  TOOLBAR_COLLECTION:      2,
  READING_LIST_COLLECTION: 3,

  /**
   * Recursively migrate a Safari collection of bookmarks.
   *
   * @param aEntries
   *        the collection's children
   * @param aCollection
   *        one of the values above.
   */
  _migrateCollection: function B__migrateCollection(aEntries, aCollection) {
    // A collection of bookmarks in Safari resembles places roots.  In the
    // property list files (Bookmarks.plist, ReadingList.plist) they are
    // stored as regular bookmarks folders, and thus can only be distinguished
    // from by their names and places in the hierarchy.

    let entriesFiltered = [];
    if (aCollection == this.ROOT_COLLECTION) {
      for (let entry of aEntries) {
        let type = entry.get("WebBookmarkType");
        if (type == "WebBookmarkTypeList" && entry.has("Children")) {
          let title = entry.get("Title");
          let children = entry.get("Children");
          if (title == "BookmarksBar")
            this._migrateCollection(children, this.TOOLBAR_COLLECTION);
          else if (title == "BookmarksMenu")
            this._migrateCollection(children, this.MENU_COLLECTION);
          else if (title == "com.apple.ReadingList")
            this._migrateCollection(children, this.READING_LIST_COLLECTION);
          else if (entry.get("ShouldOmitFromUI") !== true)
            entriesFiltered.push(entry);
        }
        else if (type == "WebBookmarkTypeLeaf") {
          entriesFiltered.push(entry);
        }
      }
    }
    else {
      entriesFiltered = aEntries;
    }

    if (entriesFiltered.length == 0)
      return;

    let folder = -1;
    switch (aCollection) {
      case this.ROOT_COLLECTION: {
        // In Safari, it is possible (though quite cumbersome) to move
        // bookmarks to the bookmarks root, which is the parent folder of
        // all bookmarks "collections".  That is somewhat in parallel with
        // both the places root and the unfiled-bookmarks root. 
        // Because the former is only an implementation detail in our UI,
        // the unfiled root seems to be the best choice.
        folder = PlacesUtils.unfiledBookmarksFolderId;
        break;
      }
      case this.MENU_COLLECTION: {
        folder = PlacesUtils.bookmarksMenuFolderId;
        if (!MigrationUtils.isStartupMigration) {
          folder = MigrationUtils.createImportedBookmarksFolder("Safari",
                                                                folder);
        }
        break;
      }
      case this.TOOLBAR_COLLECTION: {
        folder = PlacesUtils.toolbarFolderId;
        if (!MigrationUtils.isStartupMigration) {
          folder = MigrationUtils.createImportedBookmarksFolder("Safari",
                                                                folder);
        }
        break;
      }
      case this.READING_LIST_COLLECTION: {
        // Reading list items are imported as regular bookmarks.
        // They are imported under their own folder, created either under the
        // bookmarks menu (in the case of startup migration).
        folder = PlacesUtils.bookmarks.createFolder(
            PlacesUtils.bookmarksMenuFolderId,
            MigrationUtils.getLocalizedString("importedSafariReadingList"),
            PlacesUtils.bookmarks.DEFAULT_INDEX);
        break;
      }
      default:
        throw new Error("Unexpected value for aCollection!");
    }

    this._migrateEntries(entriesFiltered, folder);
  },

  // migrate the given array of safari bookmarks to the given places
  // folder.
  _migrateEntries: function B__migrateEntries(aEntries, aFolderId) {
    for (let entry of aEntries) {
      let type = entry.get("WebBookmarkType");
      if (type == "WebBookmarkTypeList" && entry.has("Children")) {
        let title = entry.get("Title");
        let folderId = PlacesUtils.bookmarks.createFolder(
           aFolderId, title, PlacesUtils.bookmarks.DEFAULT_INDEX);

        // Empty folders may not have a children array.
        if (entry.has("Children"))
          this._migrateEntries(entry.get("Children"), folderId, false);
      }
      else if (type == "WebBookmarkTypeLeaf" && entry.has("URLString")) {
        let title, uri;
        if (entry.has("URIDictionary"))
          title = entry.get("URIDictionary").get("title");

        try {
          uri = NetUtil.newURI(entry.get("URLString"));
        }
        catch(ex) {
          Cu.reportError("Invalid uri set for Safari bookmark: " + entry.get("URLString"));
        }
        if (uri) {
          PlacesUtils.bookmarks.insertBookmark(aFolderId, uri,
            PlacesUtils.bookmarks.DEFAULT_INDEX, title);
        }
      }
    }
  }
};

function History(aHistoryFile) {
  this._file = aHistoryFile;
}
History.prototype = {
  type: MigrationUtils.resourceTypes.HISTORY,

  // Helper method for converting the visit date property to a PRTime value.
  // The visit date is stored as a string, so it's not read as a Date
  // object by PropertyListUtils.
  _parseCocoaDate: function H___parseCocoaDate(aCocoaDateStr) {
    let asDouble = parseFloat(aCocoaDateStr);
    if (!isNaN(asDouble)) {
      // reference date of NSDate.
      let date = new Date("1 January 2001, GMT");
      date.setMilliseconds(asDouble * 1000);
      return date * 1000;
    }
    return 0;
  },

  migrate: function H_migrate(aCallback) {
    PropertyListUtils.read(this._file, function migrateHistory(aDict) {
      try {
        if (!aDict)
          throw new Error("Could not read history property list");
        if (!aDict.has("WebHistoryDates"))
          throw new Error("Unexpected history-property list format");

        // Safari's History file contains only top-level urls.  It does not
        // distinguish between typed urls and linked urls.
        let transType = PlacesUtils.history.TRANSITION_LINK;

        let places = [];
        let entries = aDict.get("WebHistoryDates");
        for (let entry of entries) {
          if (entry.has("lastVisitedDate")) {
            let visitDate = this._parseCocoaDate(entry.get("lastVisitedDate"));
            try {
              places.push({ uri: NetUtil.newURI(entry.get("")),
                            title: entry.get("title"),
                            visits: [{ transitionType: transType,
                                       visitDate: visitDate }] });
            }
            catch(ex) {
              // Safari's History file may contain malformed URIs which
              // will be ignored.
              Cu.reportError(ex)
            }
          }
        }
        if (places.length > 0) {
          PlacesUtils.asyncHistory.updatePlaces(places, {
            _success: false,
            handleResult: function() {
              // Importing any entry is considered a successful import.
              this._success = true;
            },
            handleError: function() {},
            handleCompletion: function() {
              aCallback(this._success);
            }
          });
        }
        else {
          aCallback(false);
        }
      }
      catch(ex) {
        Cu.reportError(ex);
        aCallback(false);
      }
    }.bind(this));
  }
};

/**
 * Safari's preferences property list is independently used for three purposes:
 * (a) importation of preferences
 * (b) importation of search strings
 * (c) retrieving the home page.
 *
 * So, rather than reading it three times, it's cached and managed here.
 */
function MainPreferencesPropertyList(aPreferencesFile) {
  this._file = aPreferencesFile;
  this._callbacks = [];
}
MainPreferencesPropertyList.prototype = {
  /**
   * @see PropertyListUtils.read
   */
  read: function MPPL_read(aCallback) {
    if ("_dict" in this) {
      aCallback(this._dict);
      return;
    }

    let alreadyReading = this._callbacks.length > 0;
    this._callbacks.push(aCallback);
    if (!alreadyReading) {
      PropertyListUtils.read(this._file, function readPrefs(aDict) {
        this._dict = aDict;
        for (let callback of this._callbacks) {
          try {
            callback(aDict);
          }
          catch(ex) {
            Cu.reportError(ex);
          }
        }
        this._callbacks.splice(0);
      }.bind(this));
    }
  },

  // Workaround for nsIBrowserProfileMigrator.sourceHomePageURL until
  // it's replaced with an async method.
  _readSync: function MPPL__readSync() {
    if ("_dict" in this)
      return this._dict;
  
    let inputStream = Cc["@mozilla.org/network/file-input-stream;1"].
                      createInstance(Ci.nsIFileInputStream);
    inputStream.init(this._file, -1, -1, 0);
    let binaryStream = Cc["@mozilla.org/binaryinputstream;1"].
                       createInstance(Ci.nsIBinaryInputStream);
    binaryStream.setInputStream(inputStream);
    let bytes = binaryStream.readByteArray(inputStream.available());
    this._dict = PropertyListUtils._readFromArrayBufferSync(
      Uint8Array(bytes).buffer);
    return this._dict;
  }
};

function Preferences(aMainPreferencesPropertyListInstance) {
  this._mainPreferencesPropertyList = aMainPreferencesPropertyListInstance;
}
Preferences.prototype = {
  type: MigrationUtils.resourceTypes.SETTINGS,

  migrate: function MPR_migrate(aCallback) {
    this._mainPreferencesPropertyList.read(
      MigrationUtils.wrapMigrateFunction(function migratePrefs(aDict) {
        if (!aDict)
          throw new Error("Could not read preferences file");

        this._dict = aDict;

        let invert = function(webkitVal) !webkitVal;
        this._set("AutoFillPasswords", "signon.rememberSignons");
        this._set("OpenNewTabsInFront", "browser.tabs.loadInBackground", invert);
        this._set("WebKitJavaScriptCanOpenWindowsAutomatically",
                   "dom.disable_open_during_load", invert);

        // layout.spellcheckDefault is a boolean stored as a number.
        this._set("WebContinuousSpellCheckingEnabled",
                  "layout.spellcheckDefault", Number);

        // Auto-load images
        // Firefox has an elaborate set of Image preferences. The correlation is:
        // Mode:                            Safari    Firefox
        // Blocked                          FALSE     2
        // Allowed                          TRUE      1
        // Allowed, originating site only   --        3
        this._set("WebKitDisplayImagesKey", "permissions.default.image",
                  function(webkitVal) webkitVal ? 1 : 2);

        // Default charset migration
        this._set("WebKitDefaultTextEncodingName", "intl.charset.default",
          function(webkitCharset) {
            // We don't support x-mac-korean (see bug 713516), but it mostly matches
            // EUC-KR.
            if (webkitCharset == "x-mac-korean")
              return "EUC-KR";

            // getCharsetAlias throws if an invalid value is passed in.
            try {
              return Cc["@mozilla.org/charset-converter-manager;1"].
                     getService(Ci.nsICharsetConverterManager).
                     getCharsetAlias(webkitCharset);
            }
            catch(ex) {
              Cu.reportError("Could not convert webkit charset '" + webkitCharset +
                             "' to a supported charset");
            }
            // Don't set the preference if we could not get the corresponding
            // charset.
            return undefined;
          });

#ifdef XP_WIN
        // Cookie-accept policy.
        // For the OS X version, see WebFoundationCookieBehavior.
        // Setting                    Safari          Firefox
        // Always Accept              0               0
        // Accept from Originating    2               1
        // Never Accept               1               2
        this._set("WebKitCookieStorageAcceptPolicy",
          "network.cookie.cookieBehavior",
          function(webkitVal) webkitVal == 0 ? 0 : webkitVal == 1 ? 2 : 1);
#endif

        this._migrateFontSettings();
        this._migrateDownloadsFolder();
    }.bind(this), aCallback));
  },

  /**
   * Attempts to migrates a preference from Safari.  Returns whether the preference
   * has been migrated.
   * @param aSafariKey
   *        The dictionary key for the preference of Safari.
   * @param aMozPref
   *        The Goanna/Pale Moon preference to which aSafariKey should be migrated
   * @param [optional] aConvertFunction(aSafariValue)
   *        a function that converts the safari-preference value to the
   *        appropriate value for aMozPref.  If it's not passed, then the
   *        Safari value is set as is.
   *        If aConvertFunction returns undefined, then aMozPref is not set
   *        at all.
   * @return whether or not aMozPref was set.
   */
  _set: function MPR_set(aSafariKey, aMozPref, aConvertFunction) {
    if (this._dict.has(aSafariKey)) {
      let safariVal = this._dict.get(aSafariKey);
      let mozVal = aConvertFunction !== undefined ?
                   aConvertFunction(safariVal) : safariVal;
      switch (typeof(mozVal)) {
        case "string":
          Services.prefs.setCharPref(aMozPref, mozVal);
          break;
        case "number":
          Services.prefs.setIntPref(aMozPref, mozVal);
          break;
        case "boolean":
          Services.prefs.setBoolPref(aMozPref, mozVal);
          break;
        case "undefined":
          return false;
        default:
          throw new Error("Unexpected value type: " + typeof(mozVal));
      }
    }
    return true;
  },

  // Fonts settings are quite problematic for migration, for a couple of
  // reasons:
  // (a) Every font preference in Goanna is set for a particular language.
  //     In Safari, each font preference applies to all languages.
  // (b) The current underlying implementation of nsIFontEnumerator cannot
  //     really tell you anything about a font: no matter what language or type
  //     you try to enumerate with EnumerateFonts, you get an array of all
  //     fonts in the systems (This also breaks our fonts dialog).
  // (c) In Goanna, each langauge has a distinct serif and sans-serif font
  //     preference.  Safari has only one default font setting.  It seems that
  //     it checks if it's a serif or sans serif font, and when a site
  //     explicitly asks to use serif/sans-serif font, it uses the default font
  //     only if it applies to this type.
  // (d) The solution of guessing the lang-group out of the default charset (as
  //     done in the old Safari migrator) can only work when:
  //     (1) The default charset preference is set.
  //     (2) It's not a unicode charset.
  // For now, we use the language implied by the system locale as the
  // lang-group. The only exception is minimal font size, which is an
  // accessibility preference in Safari (under the Advanced tab). If it is set,
  // we set it for all languages.
  // As for the font type of the default font (serif/sans-serif), the default
  // type for the given language is used (set in font.default.LANGGROUP).
  _migrateFontSettings: function MPR__migrateFontSettings() {
    // If "Never use font sizes smaller than [ ] is set", migrate it for all
    // languages.
    if (this._dict.has("WebKitMinimumFontSize")) {
      let minimumSize = this._dict.get("WebKitMinimumFontSize");
      if (typeof(minimumSize) == "number") {
        let prefs = Services.prefs.getChildList("font.minimum-size");
        for (let pref of prefs) {
          Services.prefs.setIntPref(pref, minimumSize);
        }
      }
      else {
        Cu.reportError("WebKitMinimumFontSize was set to an invalid value: " +
                       minimumSize);
      }
    }

    // In theory, the lang group could be "x-unicode". This will result
    // in setting the fonts for "Other Languages".
    let lang = this._getLocaleLangGroup();

    let anySet = false;
    let fontType = Services.prefs.getCharPref("font.default." + lang);
    anySet |= this._set("WebKitFixedFont", "font.name.monospace." + lang);
    anySet |= this._set("WebKitDefaultFixedFontSize", "font.size.fixed." + lang);
    anySet |= this._set("WebKitStandardFont",
                        "font.name." + fontType + "." + lang);
    anySet |= this._set("WebKitDefaultFontSize", "font.size.variable." + lang);

    // If we set font settings for a particular language, we'll also set the
    // fonts dialog to open with the fonts settings for that langauge.
    if (anySet)
      Services.prefs.setCharPref("font.language.group", lang);
  },

  // Get the language group for the system locale.
  _getLocaleLangGroup: function MPR__getLocaleLangGroup() {
    let locale = Services.locale.getLocaleComponentForUserAgent();

    // See nsLanguageAtomService::GetLanguageGroup
    let localeLangGroup = "x-unicode";
    let bundle = Services.strings.createBundle(
      "resource://gre/res/langGroups.properties");
    try {
      localeLangGroup = bundle.GetStringFromName(locale);
    }
    catch(ex) {
      let hyphenAt = locale.indexOf("-");
      if (hyphenAt != -1) {
        try {
          localeLangGroup = bundle.GetStringFromName(locale.substr(0, hyphenAt));
        }
        catch(ex2) { }
      }
    }
    return localeLangGroup;
  },

  _migrateDownloadsFolder: function MPR__migrateDownloadsFolder() {
    // Windows Safari uses DownloadPath while Mac uses DownloadsPath.
    // Check both for future compatibility.
    let key;
    if (this._dict.has("DownloadsPath"))
      key = "DownloadsPath";
    else if (this._dict.has("DownloadPath"))
      key = "DownloadPath";
    else
      return;

    let downloadsFolder = FileUtils.File(this._dict.get(key));

    // If the download folder is set to the Desktop or to ~/Downloads, set the
    // folderList pref appropriately so that "Desktop"/Downloads is shown with
    // pretty name in the preferences dialog.
    let folderListVal = 2;
    if (downloadsFolder.equals(FileUtils.getDir("Desk", []))) {
      folderListVal = 0;
    }
    else {
      let dnldMgr = Cc["@mozilla.org/download-manager;1"].
                    getService(Ci.nsIDownloadManager);
      if (downloadsFolder.equals(dnldMgr.defaultDownloadsDirectory))
        folderListVal = 1;
    }
    Services.prefs.setIntPref("browser.download.folderList", folderListVal);
    Services.prefs.setComplexValue("browser.download.dir", Ci.nsILocalFile,
                                   downloadsFolder);
  }
};

function SearchStrings(aMainPreferencesPropertyListInstance) {
  this._mainPreferencesPropertyList = aMainPreferencesPropertyListInstance;
}
SearchStrings.prototype = {
  type: MigrationUtils.resourceTypes.OTHERDATA,

  migrate: function SS_migrate(aCallback) {
    this._mainPreferencesPropertyList.read(MigrationUtils.wrapMigrateFunction(
      function migrateSearchStrings(aDict) {
        if (!aDict)
          throw new Error("Could not get preferences dictionary");

        if (aDict.has("RecentSearchStrings")) {
          let recentSearchStrings = aDict.get("RecentSearchStrings");
          if (recentSearchStrings && recentSearchStrings.length > 0) {
            let changes = [{op: "add",
                            fieldname: "searchbar-history",
                            value: searchString}
                           for (searchString of recentSearchStrings)];
            FormHistory.update(changes);
          }
        }
      }.bind(this), aCallback));
  }
};

#ifdef XP_MACOSX
// On OS X, the cookie-accept policy preference is stored in a separate
// property list.
// For the Windows version, check Preferences.migrate.
function WebFoundationCookieBehavior(aWebFoundationFile) {
  this._file = aWebFoundationFile;
}
WebFoundationCookieBehavior.prototype = {
  type: MigrationUtils.resourceTypes.SETTINGS,

  migrate: function WFPL_migrate(aCallback) {
    PropertyListUtils.read(this._file, MigrationUtils.wrapMigrateFunction(
      function migrateCookieBehavior(aDict) {
        if (!aDict)
          throw new Error("Could not read com.apple.WebFoundation.plist");

        if (aDict.has("NSHTTPAcceptCookies")) {
          // Setting                    Safari          Firefox
          // Always Accept              always          0
          // Accept from Originating    current page    1
          // Never Accept               never           2
          let acceptCookies = aDict.get("NSHTTPAcceptCookies");
          let cookieValue = acceptCookies == "never" ? 2 :
                            acceptCookies == "current page" ? 1 : 0;
          Services.prefs.setIntPref("network.cookie.cookieBehavior",
                                    cookieValue);
        }
      }.bind(this), aCallback));
  }
};
#endif

function SafariProfileMigrator() {
}

SafariProfileMigrator.prototype = Object.create(MigratorPrototype);

SafariProfileMigrator.prototype.getResources = function SM_getResources() {
  let profileDir =
#ifdef XP_MACOSX
    FileUtils.getDir("ULibDir", ["Safari"], false);
#else
    FileUtils.getDir("AppData", ["Apple Computer", "Safari"], false);
#endif
  if (!profileDir.exists())
    return null;

  let resources = [];
  let pushProfileFileResource = function(aFileName, aConstructor) {
    let file = profileDir.clone();
    file.append(aFileName);
    if (file.exists())
      resources.push(new aConstructor(file));
  };

  pushProfileFileResource("History.plist", History);
  pushProfileFileResource("Bookmarks.plist", Bookmarks);
  
  // The Reading List feature was introduced at the same time in Windows and
  // Mac versions of Safari.  Not surprisingly, they are stored in the same
  // format in both versions.  Surpsingly, only on Windows there is a
  // separate property list for it.  This isn't #ifdefed out on mac, because
  // Apple may fix this at some point.
  pushProfileFileResource("ReadingList.plist", Bookmarks);

  let prefsDir = 
#ifdef XP_MACOSX
    FileUtils.getDir("UsrPrfs", [], false);
#else
    FileUtils.getDir("AppData", ["Apple Computer", "Preferences"], false);
#endif

  let prefs = this.mainPreferencesPropertyList;
  if (prefs) {
    resources.push(new Preferences(prefs));
    resources.push(new SearchStrings(prefs));
  }

#ifdef XP_MACOSX
  // On OS X, the cookie-accept policy preference is stored in a separate
  // property list.
  let wfFile = FileUtils.getFile("UsrPrfs", ["com.apple.WebFoundation.plist"]);
  if (wfFile.exists())
    resources.push(new WebFoundationCookieBehavior(wfFile));
#endif

  return resources;
};

Object.defineProperty(SafariProfileMigrator.prototype, "mainPreferencesPropertyList", {
  get: function get_mainPreferencesPropertyList() {
    if (this._mainPreferencesPropertyList === undefined) {
      let file = 
#ifdef XP_MACOSX
        FileUtils.getDir("UsrPrfs", [], false);
#else
        FileUtils.getDir("AppData", ["Apple Computer", "Preferences"], false);
#endif
      if (file.exists()) {
        file.append("com.apple.Safari.plist");
        if (file.exists()) {
          return this._mainPreferencesPropertyList =
            new MainPreferencesPropertyList(file);
        }
      }
      return this._mainPreferencesPropertyList = null;
    }
    return this._mainPreferencesPropertyList;
  }
});

Object.defineProperty(SafariProfileMigrator.prototype, "sourceHomePageURL", {
  get: function get_sourceHomePageURL() {
    if (this.mainPreferencesPropertyList) {
      let dict = this.mainPreferencesPropertyList._readSync();
      if (dict.has("HomePage"))
        return dict.get("HomePage");
    }
    return "";
  }
});

SafariProfileMigrator.prototype.classDescription = "Safari Profile Migrator";
SafariProfileMigrator.prototype.contractID = "@mozilla.org/profile/migrator;1?app=browser&type=safari";
SafariProfileMigrator.prototype.classID = Components.ID("{4b609ecf-60b2-4655-9df4-dc149e474da1}");

this.NSGetFactory = XPCOMUtils.generateNSGetFactory([SafariProfileMigrator]);
