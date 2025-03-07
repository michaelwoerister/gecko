/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

var EXPORTED_SYMBOLS = ["MainProcessTarget"];

const {Target} = ChromeUtils.import("chrome://remote/content/targets/Target.jsm");
const {Session} = ChromeUtils.import("chrome://remote/content/sessions/Session.jsm");
const {RemoteAgent} = ChromeUtils.import("chrome://remote/content/RemoteAgent.jsm");

const UUIDGen = Cc["@mozilla.org/uuid-generator;1"].getService(Ci.nsIUUIDGenerator);

/**
 * The main process Target.
 *
 * Matches BrowserDevToolsAgentHost from chromium, and only support a couple of Domains:
 * https://cs.chromium.org/chromium/src/content/browser/devtools/browser_devtools_agent_host.cc?dr=CSs&g=0&l=80-91
 */
class MainProcessTarget extends Target {
  /*
   * @param Targets targets
   */
  constructor(targets) {
    super(targets, Session);

    this.type = "browser";
    this.id = UUIDGen.generateUUID().toString().slice(1, -1);

    // Define the HTTP path to query this target
    this.path = `/devtools/browser/${this.id}`;
  }

  get wsDebuggerURL() {
    const {host, port} = RemoteAgent;
    return `ws://${host}:${port}${this.path}`;
  }

  toString() {
    return `[object MainProcessTarget]`;
  }

  toJSON() {
    return {
      description: "Main process target",
      devtoolsFrontendUrl: "",
      faviconUrl: "",
      id: this.id,
      title: "Main process target",
      type: this.type,
      url: "",
      webSocketDebuggerUrl: this.wsDebuggerURL,
    };
  }
}
