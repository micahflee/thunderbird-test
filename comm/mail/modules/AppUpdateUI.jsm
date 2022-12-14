/* -*- indent-tabs-mode: nil; js-indent-level: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const EXPORTED_SYMBOLS = ["AppUpdateUI"];

const { XPCOMUtils } = ChromeUtils.import(
  "resource://gre/modules/XPCOMUtils.jsm"
);
const { clearTimeout, setTimeout } = ChromeUtils.import(
  "resource://gre/modules/Timer.jsm"
);

const lazy = {};

ChromeUtils.defineModuleGetter(
  lazy,
  "MailUtils",
  "resource:///modules/MailUtils.jsm"
);

const PREF_APP_UPDATE_UNSUPPORTED_URL = "app.update.unsupported.url";

const ANCHOR_ID = "app-update-notification-icon";
const NOTIFICATION_ID = "app-update";

XPCOMUtils.defineLazyGetter(lazy, "appUpdateBundle", function() {
  return Services.strings.createBundle(
    "chrome://messenger/locale/appUpdate.properties"
  );
});

const AppUpdateUI_Internal = {
  _timeouts: [],
  _obs: [
    "quit-application",
    "update-staged",
    "update-downloaded",
    "update-available",
    "update-error",
  ],

  init() {
    for (let topic of this._obs) {
      Services.obs.addObserver(this, topic);
    }
  },

  clearCallbacks() {
    this._timeouts.forEach(t => clearTimeout(t));
    this._timeouts = [];
  },

  addTimeout(time, callback) {
    this._timeouts.push(
      setTimeout(() => {
        this.clearCallbacks();
        callback();
      }, time)
    );
  },

  getReleaseNotesURL(update) {
    if (update && update.detailsURL) {
      return update.detailsURL;
    }
    return Services.urlFormatter.formatURLPref("app.update.url.details");
  },

  getSelectedBrowser() {
    let win = Services.wm.getMostRecentWindow("mail:3pane");
    let tabmail = win.document.getElementById("tabmail");
    return tabmail.selectedBrowser;
  },

  showNotification(browser, ...args) {
    let notifications = browser.ownerGlobal.PopupNotifications;
    return notifications.show(browser, ...args);
  },

  showRestartNotification() {
    let browser = this.getSelectedBrowser();
    let doc = browser.ownerDocument;
    let brandBundle = doc.getElementById("bundle_brand");
    let appName = brandBundle.getString("brandShortName");
    let options = {
      persistent: true,
      removeOnDismissal: true,
      hideClose: true,
    };

    doc.getElementById(
      "app-update-text"
    ).textContent = lazy.appUpdateBundle.formatStringFromName(
      "updateRestartMessage",
      [appName]
    );

    let messageString = lazy.appUpdateBundle.formatStringFromName(
      "updateRestartTitle",
      [appName]
    );

    let action = {
      label: lazy.appUpdateBundle.formatStringFromName(
        "updateRestartPrimaryButtonLabel",
        [appName]
      ),
      accessKey: lazy.appUpdateBundle.GetStringFromName(
        "updateRestartPrimaryButtonAccessKey"
      ),
      callback: () => {
        lazy.MailUtils.restartApplication();
      },
    };
    let secondaryActions = [
      {
        label: lazy.appUpdateBundle.GetStringFromName(
          "updateRestartSecondaryButtonLabel"
        ),
        accessKey: lazy.appUpdateBundle.GetStringFromName(
          "updateRestartSecondaryButtonAccessKey"
        ),
        callback: () => {},
      },
    ];

    this.showNotification(
      browser,
      NOTIFICATION_ID,
      messageString,
      ANCHOR_ID,
      action,
      secondaryActions,
      options
    );
  },

  showUpdateAvailableNotification(update) {
    let browser = this.getSelectedBrowser();
    let doc = browser.ownerDocument;
    let brandBundle = doc.getElementById("bundle_brand");
    let appName = brandBundle.getString("brandShortName");
    let options = {
      persistent: true,
      removeOnDismissal: true,
      hideClose: true,
      learnMoreURL: this.getReleaseNotesURL(update),
    };

    doc.getElementById(
      "app-update-text"
    ).textContent = lazy.appUpdateBundle.formatStringFromName(
      "updateAvailableMessage",
      [appName]
    );

    let messageString = lazy.appUpdateBundle.formatStringFromName(
      "updateAvailableTitle",
      [appName]
    );

    let action = {
      label: lazy.appUpdateBundle.GetStringFromName(
        "updateAvailablePrimaryButtonLabel"
      ),
      accessKey: lazy.appUpdateBundle.GetStringFromName(
        "updateAvailablePrimaryButtonAccessKey"
      ),
      callback: () => {
        Cc["@mozilla.org/updates/update-service;1"]
          .getService(Ci.nsIApplicationUpdateService)
          .downloadUpdate(update, true);
      },
    };
    let secondaryActions = [
      {
        label: lazy.appUpdateBundle.GetStringFromName(
          "updateAvailableSecondaryButtonLabel"
        ),
        accessKey: lazy.appUpdateBundle.GetStringFromName(
          "updateAvailableSecondaryButtonAccessKey"
        ),
        callback: () => {},
      },
    ];

    this.showNotification(
      browser,
      NOTIFICATION_ID,
      messageString,
      ANCHOR_ID,
      action,
      secondaryActions,
      options
    );
  },

  showManualUpdateNotification(update) {
    let browser = this.getSelectedBrowser();
    let doc = browser.ownerDocument;
    let brandBundle = doc.getElementById("bundle_brand");
    let appName = brandBundle.getString("brandShortName");
    let options = {
      persistent: true,
      removeOnDismissal: true,
      hideClose: true,
      learnMoreURL: this.getReleaseNotesURL(update),
    };

    doc.getElementById(
      "app-update-text"
    ).textContent = lazy.appUpdateBundle.formatStringFromName(
      "updateManualMessage",
      [appName]
    );

    let messageString = lazy.appUpdateBundle.formatStringFromName(
      "updateManualTitle",
      [appName]
    );

    let action = {
      label: lazy.appUpdateBundle.formatStringFromName(
        "updateManualPrimaryButtonLabel",
        [appName]
      ),
      accessKey: lazy.appUpdateBundle.GetStringFromName(
        "updateManualPrimaryButtonAccessKey"
      ),
      callback: () => {
        let url = Services.urlFormatter.formatURLPref("app.update.url.manual");
        Cc["@mozilla.org/uriloader/external-protocol-service;1"]
          .getService(Ci.nsIExternalProtocolService)
          .loadURI(Services.io.newURI(url));
      },
    };
    let secondaryActions = [
      {
        label: lazy.appUpdateBundle.GetStringFromName(
          "updateManualSecondaryButtonLabel"
        ),
        accessKey: lazy.appUpdateBundle.GetStringFromName(
          "updateManualSecondaryButtonAccessKey"
        ),
        callback: () => {},
      },
    ];

    this.showNotification(
      browser,
      NOTIFICATION_ID,
      messageString,
      ANCHOR_ID,
      action,
      secondaryActions,
      options
    );
  },

  showUnsupportedUpdateNotification(update) {
    if (!update || !update.detailsURL) {
      Cu.reportError(
        "The update for an unsupported notification must have a " +
          "detailsURL attribute."
      );
      return;
    }

    let url = update.detailsURL;
    // If the system unsupported notification has already been shown don't show
    // it again to avoid annoying users. The unsupported message will always be
    // displayed in the About dialog.
    if (
      url == Services.prefs.getCharPref(PREF_APP_UPDATE_UNSUPPORTED_URL, null)
    ) {
      return;
    }

    Services.prefs.setCharPref(PREF_APP_UPDATE_UNSUPPORTED_URL, url);
    let browser = this.getSelectedBrowser();
    let doc = browser.ownerDocument;
    let brandBundle = doc.getElementById("bundle_brand");
    let appName = brandBundle.getString("brandShortName");
    let options = {
      persistent: true,
      removeOnDismissal: true,
      hideClose: true,
      popupIconClass: "updates-unsupported",
    };

    doc.getElementById(
      "app-update-text"
    ).textContent = lazy.appUpdateBundle.formatStringFromName(
      "updateUnsupportedMessage",
      [appName]
    );

    let messageString = lazy.appUpdateBundle.formatStringFromName(
      "updateUnsupportedTitle",
      [appName]
    );

    let action = {
      label: lazy.appUpdateBundle.formatStringFromName(
        "updateUnsupportedPrimaryButtonLabel",
        [appName]
      ),
      accessKey: lazy.appUpdateBundle.GetStringFromName(
        "updateUnsupportedPrimaryButtonAccessKey"
      ),
      callback: () => {
        Cc["@mozilla.org/uriloader/external-protocol-service;1"]
          .getService(Ci.nsIExternalProtocolService)
          .loadURI(Services.io.newURI(url));
      },
    };
    let secondaryActions = [
      {
        label: lazy.appUpdateBundle.GetStringFromName(
          "updateManualSecondaryButtonLabel"
        ),
        accessKey: lazy.appUpdateBundle.GetStringFromName(
          "updateManualSecondaryButtonAccessKey"
        ),
        callback: () => {},
      },
    ];

    this.showNotification(
      browser,
      NOTIFICATION_ID,
      messageString,
      ANCHOR_ID,
      action,
      secondaryActions,
      options
    );
  },

  handleUpdateError(update, status) {
    switch (status) {
      case "download-attempt-failed":
        this.clearCallbacks();
        this.showUpdateAvailableNotification(update);
        break;
      case "download-attempts-exceeded":
        this.clearCallbacks();
        this.showManualUpdateNotification(update);
        break;
      case "elevation-attempt-failed":
        this.clearCallbacks();
        this.showRestartNotification();
        break;
      case "elevation-attempts-exceeded":
        this.clearCallbacks();
        this.showManualUpdateNotification(update);
        break;
      case "check-attempts-exceeded":
      case "unknown":
      case "bad-perms":
        // Background update has failed, let's show the UI responsible for
        // prompting the user to update manually.
        this.clearCallbacks();
        this.showManualUpdateNotification(update);
        break;
    }
  },

  handleUpdateStagedOrDownloaded(update, status) {
    switch (status) {
      case "applied":
      case "pending":
      case "applied-service":
      case "pending-service":
      case "pending-elevate":
      case "success":
        this.clearCallbacks();

        let doorhangerWaitTimeMs =
          update && update.promptWaitTime ? update.promptWaitTime * 1000 : 0;
        this.addTimeout(doorhangerWaitTimeMs, () => {
          this.showRestartNotification();
        });
        break;
    }
  },

  handleUpdateAvailable(update, status) {
    switch (status) {
      case "show-prompt":
        this.clearCallbacks();
        this.showUpdateAvailableNotification(update);
        break;
      case "cant-apply":
        this.clearCallbacks();
        this.showManualUpdateNotification(update, false);
        break;
      case "unsupported":
        this.clearCallbacks();
        this.showUnsupportedUpdateNotification(update, false);
        break;
    }
  },

  observe(subject, topic, status) {
    let update = subject && subject.QueryInterface(Ci.nsIUpdate);
    switch (topic) {
      case "quit-application":
        for (let observer of this._obs) {
          Services.obs.removeObserver(this, observer);
        }
        break;
      case "update-available":
        if (status != "unsupported") {
          // An update check has found an update so clear the unsupported pref
          // in case it is set.
          Services.prefs.clearUserPref(PREF_APP_UPDATE_UNSUPPORTED_URL);
        }
        this.handleUpdateAvailable(update, status);
        break;
      case "update-staged":
      case "update-downloaded":
        // An update check has found an update and downloaded / staged the
        // update so clear the unsupported pref in case it is set.
        Services.prefs.clearUserPref(PREF_APP_UPDATE_UNSUPPORTED_URL);
        this.handleUpdateStagedOrDownloaded(update, status);
        break;
      case "update-error":
        this.handleUpdateError(update, status);
        break;
    }
  },
};

const AppUpdateUI = {
  init() {
    AppUpdateUI_Internal.init();
  },
};
