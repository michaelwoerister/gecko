/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
"use strict";

var EXPORTED_SYMBOLS = ["BrowserTabParent"];

class BrowserTabParent extends JSWindowActorParent {
  receiveMessage(message) {
    let browser = this.manager.browsingContext.embedderElement;
    if (!browser) {
      return; // Can happen sometimes if browser is being destroyed
    }

    let gBrowser = browser.ownerGlobal.gBrowser;
    if (!gBrowser) {
      // Note: gBrowser might be null because this message might be received
      // from the extension process. There's still an embedderElement involved,
      // but it's the one coming from dummy.xul.
      // This should probably be fixed by adding support to specifying "group: 'browsers"
      // in the registerWindowActor options/. See bug 1557118.
      return;
    }

    switch (message.name) {
      case "Browser:WindowCreated": {
        gBrowser.announceWindowCreated(browser, message.data.userContextId);
        break;
      }

      case "Browser:FirstPaint": {
        browser.ownerGlobal.gBrowserInit._firstBrowserPaintDeferred.resolve();
        break;
      }

      case "MozDOMPointerLock:Entered": {
        browser.ownerGlobal.PointerLock.entered(message.data.originNoSuffix);
        break;
      }

      case "MozDOMPointerLock:Exited":
        browser.ownerGlobal.PointerLock.exited();
        break;
    }
  }
}
