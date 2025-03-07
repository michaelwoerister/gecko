/**
 * Test for LoginManagerContent.getUserNameAndPasswordFields
 */

"use strict";

XPCOMUtils.defineLazyGlobalGetters(this, ["URL"]);

const LMCBackstagePass = ChromeUtils.import("resource://gre/modules/LoginManagerContent.jsm", null);
const { LoginManagerContent } = LMCBackstagePass;
const TESTCASES = [
  {
    description: "1 password field outside of a <form>",
    document: `<input id="pw1" type=password>`,
    returnedFieldIDs: [null, "pw1", null],
  },
  {
    description: "1 text field outside of a <form> without a password field",
    document: `<input id="un1">`,
    returnedFieldIDs: [null, null, null],
  },
  {
    description: "1 username & password field outside of a <form>",
    document: `<input id="un1">
      <input id="pw1" type=password>`,
    returnedFieldIDs: ["un1", "pw1", null],
  },
  {
    description: "1 username & password field in a <form>",
    document: `<form>
      <input id="un1">
      <input id="pw1" type=password>
      </form>`,
    returnedFieldIDs: ["un1", "pw1", null],
  },
  {
    description: "4 empty password fields outside of a <form>",
    document: `<input id="pw1" type=password>
      <input id="pw2" type=password>
      <input id="pw3" type=password>
      <input id="pw4" type=password>`,
    returnedFieldIDs: [null, null, null],
  },
  {
    description: "Form with 1 password field",
    document: `<form><input id="pw1" type=password></form>`,
    returnedFieldIDs: [null, "pw1", null],
  },
  {
    description: "Form with 2 password fields",
    document: `<form><input id="pw1" type=password><input id='pw2' type=password></form>`,
    returnedFieldIDs: [null, "pw1", null],
  },
  {
    description: "1 password field in a form, 1 outside (not processed)",
    document: `<form><input id="pw1" type=password></form><input id="pw2" type=password>`,
    returnedFieldIDs: [null, "pw1", null],
  },
  {
    description: "1 password field in a form, 1 text field outside (not processed)",
    document: `<form><input id="pw1" type=password></form><input>`,
    returnedFieldIDs: [null, "pw1", null],
  },
  {
    description: "1 text field in a form, 1 password field outside (not processed)",
    document: `<form><input></form><input id="pw1" type=password>`,
    returnedFieldIDs: [null, null, null],
  },
  {
    description: "2 password fields outside of a <form> with 1 linked via @form",
    document: `<input id="pw1" type=password><input id="pw2" type=password form='form1'>
      <form id="form1"></form>`,
    returnedFieldIDs: [null, "pw1", null],
  },
];

for (let tc of TESTCASES) {
  info("Sanity checking the testcase: " + tc.description);

  (function() {
    let testcase = tc;
    add_task(async function() {
      info("Starting testcase: " + testcase.description);
      let document = MockDocument.createTestDocument("http://localhost:8080/test/",
                                                     testcase.document);

      let input = document.querySelector("input");
      MockDocument.mockOwnerDocumentProperty(input, document, "http://localhost:8080/test/");
      MockDocument.mockNodePrincipalProperty(input, "http://localhost:8080/test/");

      // Additional mock to cache recipes
      let win = {};
      Object.defineProperty(document, "defaultView", {
        value: win,
      });
      let formOrigin = LoginHelper.getLoginOrigin(document.documentURI);
      LoginRecipesContent.cacheRecipes(formOrigin, win, new Set());

      let actual = LoginManagerContent.getUserNameAndPasswordFields(input);

      Assert.strictEqual(testcase.returnedFieldIDs.length, 3,
                         "getUserNameAndPasswordFields returns 3 elements");

      for (let i = 0; i < testcase.returnedFieldIDs.length; i++) {
        let expectedID = testcase.returnedFieldIDs[i];
        if (expectedID === null) {
          Assert.strictEqual(actual[i], expectedID,
                             "Check returned field " + i + " is null");
        } else {
          Assert.strictEqual(actual[i].id, expectedID,
                             "Check returned field " + i + " ID");
        }
      }
    });
  })();
}
