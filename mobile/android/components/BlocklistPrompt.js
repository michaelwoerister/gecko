/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const {XPCOMUtils} = ChromeUtils.import("resource://gre/modules/XPCOMUtils.jsm");
const {Services} = ChromeUtils.import("resource://gre/modules/Services.jsm");

// -----------------------------------------------------------------------
// BlocklistPrompt Service
// -----------------------------------------------------------------------


function BlocklistPrompt() {
  this.wrappedJSObject = this;
}

BlocklistPrompt.prototype = {
  prompt: function(aAddons, aCount) {
    let win = Services.wm.getMostRecentWindow("navigator:browser");
    if (win.ExtensionsView.visible) {
      win.ExtensionsView.showRestart("blocked");
    } else {
      let bundle = Services.strings.createBundle("chrome://browser/locale/browser.properties");
      let notifyBox = win.getNotificationBox();
      let restartCallback = function(aNotification, aDescription) {
        // Notify all windows that an application quit has been requested
        var cancelQuit = Cc["@mozilla.org/supports-PRBool;1"].createInstance(Ci.nsISupportsPRBool);
        Services.obs.notifyObservers(cancelQuit, "quit-application-requested", "restart");

        // If nothing aborted, quit the app
        if (!cancelQuit.data) {
          Services.startup.quit(Ci.nsIAppStartup.eRestart | Ci.nsIAppStartup.eAttemptQuit);
        }
      };

      let buttons = [{accessKey: null,
                      label: bundle.GetStringFromName("notificationRestart.button"),
                      callback: restartCallback}];
      notifyBox.appendNotification(bundle.GetStringFromName("notificationRestart.blocked"),
                                   "blocked-add-on",
                                   "",
                                   "PRIORITY_CRITICAL_HIGH",
                                   buttons);
    }
    // Disable softblocked items automatically
    for (let i = 0; i < aAddons.length; i++) {
      if (aAddons[i].item instanceof Ci.nsIPluginTag)
        aAddons[i].item.disabled = true;
      else
        aAddons[i].item.disable();
    }
  },
  classID: Components.ID("{4e6ea350-b09a-11df-94e2-0800200c9a66}"),
  QueryInterface: ChromeUtils.generateQI([]),
};

this.NSGetFactory = XPCOMUtils.generateNSGetFactory([BlocklistPrompt]);
