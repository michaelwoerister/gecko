/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set sw=2 ts=8 et tw=80 : */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// HttpLog.h should generally be included first
#include "HttpLog.h"

// Log on level :5, instead of default :4.
#undef LOG
#define LOG(args) LOG5(args)
#undef LOG_ENABLED
#define LOG_ENABLED() LOG5_ENABLED()

#include "nsHttpConnectionInfo.h"

#include "mozilla/net/DNS.h"
#include "nsComponentManagerUtils.h"
#include "nsICryptoHash.h"
#include "nsIProtocolProxyService.h"
#include "nsNetCID.h"
#include "prnetdb.h"

static nsresult SHA256(const char* aPlainText, nsAutoCString& aResult) {
  nsresult rv;
  nsCOMPtr<nsICryptoHash> hasher =
      do_CreateInstance(NS_CRYPTO_HASH_CONTRACTID, &rv);
  if (NS_FAILED(rv)) {
    LOG(("nsHttpDigestAuth: no crypto hash!\n"));
    return rv;
  }
  rv = hasher->Init(nsICryptoHash::SHA256);
  NS_ENSURE_SUCCESS(rv, rv);
  rv = hasher->Update((unsigned char*)aPlainText, strlen(aPlainText));
  NS_ENSURE_SUCCESS(rv, rv);
  return hasher->Finish(false, aResult);
}

namespace mozilla {
namespace net {

nsHttpConnectionInfo::nsHttpConnectionInfo(
    const nsACString& originHost, int32_t originPort,
    const nsACString& npnToken, const nsACString& username,
    const nsACString& topWindowOrigin, nsProxyInfo* proxyInfo,
    const OriginAttributes& originAttributes, bool endToEndSSL, bool isolated)
    : mRoutedPort(443), mIsolated(isolated), mLessThanTls13(false) {
  Init(originHost, originPort, npnToken, username, topWindowOrigin, proxyInfo,
       originAttributes, endToEndSSL);
}

nsHttpConnectionInfo::nsHttpConnectionInfo(
    const nsACString& originHost, int32_t originPort,
    const nsACString& npnToken, const nsACString& username,
    const nsACString& topWindowOrigin, nsProxyInfo* proxyInfo,
    const OriginAttributes& originAttributes, bool endToEndSSL)
    : nsHttpConnectionInfo(originHost, originPort, npnToken, username,
                           topWindowOrigin, proxyInfo, originAttributes,
                           endToEndSSL, false) {}

nsHttpConnectionInfo::nsHttpConnectionInfo(
    const nsACString& originHost, int32_t originPort,
    const nsACString& npnToken, const nsACString& username,
    const nsACString& topWindowOrigin, nsProxyInfo* proxyInfo,
    const OriginAttributes& originAttributes, const nsACString& routedHost,
    int32_t routedPort, bool isolated)
    : mIsolated(isolated), mLessThanTls13(false) {
  mEndToEndSSL = true;  // so DefaultPort() works
  mRoutedPort = routedPort == -1 ? DefaultPort() : routedPort;

  if (!originHost.Equals(routedHost) || (originPort != routedPort)) {
    mRoutedHost = routedHost;
  }
  Init(originHost, originPort, npnToken, username, topWindowOrigin, proxyInfo,
       originAttributes, true);
}

nsHttpConnectionInfo::nsHttpConnectionInfo(
    const nsACString& originHost, int32_t originPort,
    const nsACString& npnToken, const nsACString& username,
    const nsACString& topWindowOrigin, nsProxyInfo* proxyInfo,
    const OriginAttributes& originAttributes, const nsACString& routedHost,
    int32_t routedPort)
    : nsHttpConnectionInfo(originHost, originPort, npnToken, username,
                           topWindowOrigin, proxyInfo, originAttributes,
                           routedHost, routedPort, false) {}

void nsHttpConnectionInfo::Init(const nsACString& host, int32_t port,
                                const nsACString& npnToken,
                                const nsACString& username,
                                const nsACString& topWindowOrigin,
                                nsProxyInfo* proxyInfo,
                                const OriginAttributes& originAttributes,
                                bool e2eSSL) {
  LOG(("Init nsHttpConnectionInfo @%p\n", this));

  mUsername = username;
  mTopWindowOrigin = topWindowOrigin;
  mProxyInfo = proxyInfo;
  mEndToEndSSL = e2eSSL;
  mUsingConnect = false;
  mNPNToken = npnToken;
  mOriginAttributes = originAttributes;
  mTlsFlags = 0x0;
  mIsTrrServiceChannel = false;
  mTrrDisabled = false;
  mIPv4Disabled = false;
  mIPv6Disabled = false;

  mUsingHttpsProxy = (proxyInfo && proxyInfo->IsHTTPS());
  mUsingHttpProxy = mUsingHttpsProxy || (proxyInfo && proxyInfo->IsHTTP());

  if (mUsingHttpProxy) {
    mUsingConnect = mEndToEndSSL;  // SSL always uses CONNECT
    uint32_t resolveFlags = 0;
    if (NS_SUCCEEDED(mProxyInfo->GetResolveFlags(&resolveFlags)) &&
        resolveFlags & nsIProtocolProxyService::RESOLVE_ALWAYS_TUNNEL) {
      mUsingConnect = true;
    }
  }

  SetOriginServer(host, port);
}

void nsHttpConnectionInfo::BuildHashKey() {
  //
  // build hash key:
  //
  // the hash key uniquely identifies the connection type.  two connections
  // are "equal" if they end up talking the same protocol to the same server
  // and are both used for anonymous or non-anonymous connection only;
  // anonymity of the connection is setup later from nsHttpChannel::AsyncOpen
  // where we know we use anonymous connection (LOAD_ANONYMOUS load flag)
  //

  const char* keyHost;
  int32_t keyPort;

  if (mUsingHttpProxy && !mUsingConnect) {
    keyHost = ProxyHost();
    keyPort = ProxyPort();
  } else {
    keyHost = Origin();
    keyPort = OriginPort();
  }

  // The hashkey has 4 fields followed by host connection info
  // byte 0 is P/T/. {P,T} for Plaintext/TLS Proxy over HTTP
  // byte 1 is S/. S is for end to end ssl such as https:// uris
  // byte 2 is A/. A is for an anonymous channel (no cookies, etc..)
  // byte 3 is P/. P is for a private browising channel
  // byte 4 is I/. I is for insecure scheme on TLS for http:// uris
  // byte 5 is X/. X is for disallow_spdy flag
  // byte 6 is C/. C is for be Conservative
  // byte 7 is i/. i is for isolated

  mHashKey.AssignLiteral("........[tlsflags0x00000000]");
  if (mIsolated) {
    mHashKey.SetCharAt('i', 7);
  }

  mHashKey.Append(keyHost);
  mHashKey.Append(':');
  mHashKey.AppendInt(keyPort);
  if (!mUsername.IsEmpty()) {
    mHashKey.Append('[');
    mHashKey.Append(mUsername);
    mHashKey.Append(']');
  }

  if (mUsingHttpsProxy) {
    mHashKey.SetCharAt('T', 0);
  } else if (mUsingHttpProxy) {
    mHashKey.SetCharAt('P', 0);
  }
  if (mEndToEndSSL) {
    mHashKey.SetCharAt('S', 1);
  }

  // NOTE: for transparent proxies (e.g., SOCKS) we need to encode the proxy
  // info in the hash key (this ensures that we will continue to speak the
  // right protocol even if our proxy preferences change).
  //
  // NOTE: for SSL tunnels add the proxy information to the cache key.
  // We cannot use the proxy as the host parameter (as we do for non SSL)
  // because this is a single host tunnel, but we need to include the proxy
  // information so that a change in proxy config will mean this connection
  // is not reused

  // NOTE: Adding the username and the password provides a means to isolate
  // keep-alive to the URL bar domain as well: If the username is the URL bar
  // domain, keep-alive connections are not reused by resources bound to
  // different URL bar domains as the respective hash keys are not matching.

  if ((!mUsingHttpProxy && ProxyHost()) || (mUsingHttpProxy && mUsingConnect)) {
    mHashKey.AppendLiteral(" (");
    mHashKey.Append(ProxyType());
    mHashKey.Append(':');
    mHashKey.Append(ProxyHost());
    mHashKey.Append(':');
    mHashKey.AppendInt(ProxyPort());
    mHashKey.Append(')');
    mHashKey.Append('[');
    mHashKey.Append(ProxyUsername());
    mHashKey.Append(':');
    const char* password = ProxyPassword();
    if (strlen(password) > 0) {
      nsAutoCString digestedPassword;
      nsresult rv = SHA256(password, digestedPassword);
      if (rv == NS_OK) {
        mHashKey.Append(digestedPassword);
      }
    }
    mHashKey.Append(']');
  }

  if (!mRoutedHost.IsEmpty()) {
    mHashKey.AppendLiteral(" <ROUTE-via ");
    mHashKey.Append(mRoutedHost);
    mHashKey.Append(':');
    mHashKey.AppendInt(mRoutedPort);
    mHashKey.Append('>');
  }

  if (!mNPNToken.IsEmpty()) {
    mHashKey.AppendLiteral(" {NPN-TOKEN ");
    mHashKey.Append(mNPNToken);
    mHashKey.AppendLiteral("}");
  }

  if (GetTrrDisabled()) {
    // When connecting with TRR disabled, we enforce a separate connection
    // hashkey so that we also can trigger a fresh DNS resolver that then
    // doesn't use TRR as the previous connection might have.
    mHashKey.AppendLiteral("[NOTRR]");
  }

  if (GetIPv4Disabled()) {
    mHashKey.AppendLiteral("[!v4]");
  }

  if (GetIPv6Disabled()) {
    mHashKey.AppendLiteral("[!v6]");
  }

  if (mIsolated && !mTopWindowOrigin.IsEmpty()) {
    mHashKey.Append('{');
    mHashKey.Append('{');
    mHashKey.Append(mTopWindowOrigin);
    mHashKey.Append('}');
    mHashKey.Append('}');
  }

  if (mProxyInfo) {
    const nsCString& connectionIsolationKey =
        mProxyInfo->ConnectionIsolationKey();
    if (!connectionIsolationKey.IsEmpty()) {
      mHashKey.AppendLiteral("{CIK ");
      mHashKey.Append(connectionIsolationKey);
      mHashKey.AppendLiteral("}");
    }
  }

  nsAutoCString originAttributes;
  mOriginAttributes.CreateSuffix(originAttributes);
  mHashKey.Append(originAttributes);
}

void nsHttpConnectionInfo::SetOriginServer(const nsACString& host,
                                           int32_t port) {
  mOrigin = host;
  mOriginPort = port == -1 ? DefaultPort() : port;
  BuildHashKey();
}

already_AddRefed<nsHttpConnectionInfo> nsHttpConnectionInfo::Clone() const {
  RefPtr<nsHttpConnectionInfo> clone;
  if (mRoutedHost.IsEmpty()) {
    clone = new nsHttpConnectionInfo(
        mOrigin, mOriginPort, mNPNToken, mUsername, mTopWindowOrigin,
        mProxyInfo, mOriginAttributes, mEndToEndSSL, mIsolated);
  } else {
    MOZ_ASSERT(mEndToEndSSL);
    clone = new nsHttpConnectionInfo(
        mOrigin, mOriginPort, mNPNToken, mUsername, mTopWindowOrigin,
        mProxyInfo, mOriginAttributes, mRoutedHost, mRoutedPort, mIsolated);
  }

  // Make sure the anonymous, insecure-scheme, and private flags are transferred
  clone->SetAnonymous(GetAnonymous());
  clone->SetPrivate(GetPrivate());
  clone->SetInsecureScheme(GetInsecureScheme());
  clone->SetNoSpdy(GetNoSpdy());
  clone->SetBeConservative(GetBeConservative());
  clone->SetTlsFlags(GetTlsFlags());
  clone->SetIsTrrServiceChannel(GetIsTrrServiceChannel());
  clone->SetTrrDisabled(GetTrrDisabled());
  clone->SetIPv4Disabled(GetIPv4Disabled());
  clone->SetIPv6Disabled(GetIPv6Disabled());
  MOZ_ASSERT(clone->Equals(this));

  return clone.forget();
}

void nsHttpConnectionInfo::CloneAsDirectRoute(nsHttpConnectionInfo** outCI) {
  if (mRoutedHost.IsEmpty()) {
    RefPtr<nsHttpConnectionInfo> clone = Clone();
    clone.forget(outCI);
    return;
  }

  RefPtr<nsHttpConnectionInfo> clone = new nsHttpConnectionInfo(
      mOrigin, mOriginPort, EmptyCString(), mUsername, mTopWindowOrigin,
      mProxyInfo, mOriginAttributes, mEndToEndSSL, mIsolated);
  // Make sure the anonymous, insecure-scheme, and private flags are transferred
  clone->SetAnonymous(GetAnonymous());
  clone->SetPrivate(GetPrivate());
  clone->SetInsecureScheme(GetInsecureScheme());
  clone->SetNoSpdy(GetNoSpdy());
  clone->SetBeConservative(GetBeConservative());
  clone->SetTlsFlags(GetTlsFlags());
  clone->SetIsTrrServiceChannel(GetIsTrrServiceChannel());
  clone->SetTrrDisabled(GetTrrDisabled());
  clone->SetIPv4Disabled(GetIPv4Disabled());
  clone->SetIPv6Disabled(GetIPv6Disabled());

  clone.forget(outCI);
}

nsresult nsHttpConnectionInfo::CreateWildCard(nsHttpConnectionInfo** outParam) {
  // T???mozilla.org:443 (https:proxy.ducksong.com:3128) [specifc form]
  // TS??*:0 (https:proxy.ducksong.com:3128)   [wildcard form]

  if (!mUsingHttpsProxy) {
    MOZ_ASSERT(false);
    return NS_ERROR_NOT_IMPLEMENTED;
  }

  RefPtr<nsHttpConnectionInfo> clone;
  clone = new nsHttpConnectionInfo(NS_LITERAL_CSTRING("*"), 0, mNPNToken,
                                   mUsername, mTopWindowOrigin, mProxyInfo,
                                   mOriginAttributes, true);
  // Make sure the anonymous and private flags are transferred!
  clone->SetAnonymous(GetAnonymous());
  clone->SetPrivate(GetPrivate());
  clone.forget(outParam);
  return NS_OK;
}

void nsHttpConnectionInfo::SetTrrDisabled(bool aNoTrr) {
  if (mTrrDisabled != aNoTrr) {
    mTrrDisabled = aNoTrr;
    BuildHashKey();
  }
}

void nsHttpConnectionInfo::SetIPv4Disabled(bool aNoIPv4) {
  if (mIPv4Disabled != aNoIPv4) {
    mIPv4Disabled = aNoIPv4;
    BuildHashKey();
  }
}

void nsHttpConnectionInfo::SetIPv6Disabled(bool aNoIPv6) {
  if (mIPv6Disabled != aNoIPv6) {
    mIPv6Disabled = aNoIPv6;
    BuildHashKey();
  }
}

void nsHttpConnectionInfo::SetTlsFlags(uint32_t aTlsFlags) {
  mTlsFlags = aTlsFlags;

  mHashKey.Replace(19, 8, nsPrintfCString("%08x", mTlsFlags));
}

bool nsHttpConnectionInfo::UsingProxy() {
  if (!mProxyInfo) return false;
  return !mProxyInfo->IsDirect();
}

bool nsHttpConnectionInfo::HostIsLocalIPLiteral() const {
  PRNetAddr prAddr;
  // If the host/proxy host is not an IP address literal, return false.
  if (ProxyHost()) {
    if (PR_StringToNetAddr(ProxyHost(), &prAddr) != PR_SUCCESS) {
      return false;
    }
  } else if (PR_StringToNetAddr(Origin(), &prAddr) != PR_SUCCESS) {
    return false;
  }
  NetAddr netAddr;
  PRNetAddrToNetAddr(&prAddr, &netAddr);
  return IsIPAddrLocal(&netAddr);
}

}  // namespace net
}  // namespace mozilla
