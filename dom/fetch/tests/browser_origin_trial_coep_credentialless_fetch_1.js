const TOP_LEVEL_URL =
  getRootDirectory(gTestPath).replace(
    "chrome://mochitests/content",
    "https://example.com"
  ) + "open_credentialless_document.sjs";

const SAME_ORIGIN = "https://example.com";
const CROSS_ORIGIN = "https://test1.example.com";

const USE_CREDENTIALLESS = true;
const NO_CREDENTIALLESS = false;

const GET_STATE_URL =
  getRootDirectory(gTestPath).replace(
    "chrome://mochitests/content",
    "https://example.com"
  ) + "store_header.sjs?getstate";

async function addCookieToOrigin(origin) {
  const fetchRequestURL =
    getRootDirectory(gTestPath).replace("chrome://mochitests/content", origin) +
    "store_header.sjs?addcookie";

  const addcookieTab = await BrowserTestUtils.openNewForegroundTab(
    gBrowser,
    fetchRequestURL
  );

  await SpecialPowers.spawn(addcookieTab.linkedBrowser, [], async function() {
    content.document.cookie = "coep=credentialless; SameSite=None; Secure";
  });
  await BrowserTestUtils.removeTab(addcookieTab);
}

async function testOrigin(
  fetchOrigin,
  isCredentialless,
  fetchRequestMode,
  fetchRequestCrendentials,
  expectedCookieResult
) {
  let topLevelUrl = TOP_LEVEL_URL;
  if (isCredentialless) {
    topLevelUrl += "?credentialless";
  }

  const noCredentiallessTab = await BrowserTestUtils.openNewForegroundTab(
    gBrowser,
    topLevelUrl
  );

  const fetchRequestURL =
    getRootDirectory(gTestPath).replace(
      "chrome://mochitests/content",
      fetchOrigin
    ) + "store_header.sjs?checkheader";

  await SpecialPowers.spawn(
    noCredentiallessTab.linkedBrowser,
    [
      fetchRequestURL,
      fetchRequestMode,
      fetchRequestCrendentials,
      GET_STATE_URL,
      expectedCookieResult,
    ],
    async function(
      fetchRequestURL,
      fetchRequestMode,
      fetchRequestCrendentials,
      getStateURL,
      expectedCookieResult
    ) {
      // When store_header.sjs receives this request, it will store
      // whether it has received the cookie as a shared state.
      await content.fetch(fetchRequestURL, {
        mode: fetchRequestMode,
        credentials: fetchRequestCrendentials,
      });

      // This request is used to get the saved state from the
      // previous fetch request.
      const response = await content.fetch(getStateURL, {
        mode: "cors",
      });
      const text = await response.text();
      is(text, expectedCookieResult);
    }
  );

  await BrowserTestUtils.removeTab(noCredentiallessTab);
}

async function doTest(
  origin,
  fetchRequestMode,
  fetchRequestCrendentials,
  expectedCookieResultForNoCredentialless,
  expectedCookieResultForCredentialless
) {
  await testOrigin(
    origin,
    USE_CREDENTIALLESS,
    fetchRequestMode,
    fetchRequestCrendentials,
    expectedCookieResultForCredentialless
  );
  await testOrigin(
    origin,
    NO_CREDENTIALLESS,
    fetchRequestMode,
    fetchRequestCrendentials,
    expectedCookieResultForNoCredentialless
  );
}

add_task(async function() {
  await SpecialPowers.pushPrefEnv({
    set: [
      ["browser.tabs.remote.coep.credentialless", false],
      ["dom.origin-trials.enabled", true],
      ["dom.origin-trials.test-key.enabled", true],
    ],
  });

  await addCookieToOrigin(SAME_ORIGIN);
  await addCookieToOrigin(CROSS_ORIGIN);

  // Cookies never sent with omit
  await doTest(SAME_ORIGIN, "no-cors", "omit", "noCookie", "noCookie");
  await doTest(SAME_ORIGIN, "cors", "omit", "noCookie", "noCookie");
  await doTest(CROSS_ORIGIN, "no-cors", "omit", "noCookie", "noCookie");
  await doTest(CROSS_ORIGIN, "cors", "omit", "noCookie", "noCookie");
});
