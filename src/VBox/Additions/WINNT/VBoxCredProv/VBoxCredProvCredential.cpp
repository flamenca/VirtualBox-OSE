/* $Id$ */
/** @file
 * VBoxCredProvCredential - Class for keeping and handling the passed credentials.
 */

/*
 * Copyright (C) 2012 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

/*******************************************************************************
*   Header Files                                                               *
*******************************************************************************/
#ifndef WIN32_NO_STATUS
# include <ntstatus.h>
# define WIN32_NO_STATUS
#endif
#include <intsafe.h>

#include "VBoxCredentialProvider.h"

#include "VBoxCredProvProvider.h"
#include "VBoxCredProvCredential.h"
#include "VBoxCredProvUtils.h"

#include <lm.h>

#include <iprt/initterm.h>
#include <iprt/mem.h>
#include <iprt/string.h>




VBoxCredProvCredential::VBoxCredProvCredential(void) :
    m_enmUsageScenario(CPUS_INVALID),
    m_cRefs(1),
    m_pEvents(NULL),
    m_fHaveCreds(false)
{
    VBoxCredProvVerbose(0, "VBoxCredProvCredential: Created\n");
    VBoxCredentialProviderAcquire();
    RT_BZERO(m_apwszCredentials, sizeof(PRTUTF16) * VBOXCREDPROV_NUM_FIELDS);
}


VBoxCredProvCredential::~VBoxCredProvCredential(void)
{
    VBoxCredProvVerbose(0, "VBoxCredProvCredential: Destroying\n");
    Reset();
    VBoxCredentialProviderRelease();
}


ULONG
VBoxCredProvCredential::AddRef(void)
{
    LONG cRefs = InterlockedIncrement(&m_cRefs);
    VBoxCredProvVerbose(0, "VBoxCredProvCredential::AddRef: Returning refcount=%ld\n",
                        cRefs);
    return cRefs;
}


ULONG
VBoxCredProvCredential::Release(void)
{
    LONG cRefs = InterlockedDecrement(&m_cRefs);
    VBoxCredProvVerbose(0, "VBoxCredProvCredential::Release: Returning refcount=%ld\n",
                        cRefs);
    if (!cRefs)
    {
        VBoxCredProvVerbose(0, "VBoxCredProvCredential: Calling destructor\n");
        delete this;
    }
    return cRefs;
}


HRESULT
VBoxCredProvCredential::QueryInterface(REFIID interfaceID, void **ppvInterface)
{
    HRESULT hr = S_OK;;
    if (ppvInterface)
    {
        if (   IID_IUnknown                      == interfaceID
            || IID_ICredentialProviderCredential == interfaceID)
        {
            *ppvInterface = static_cast<IUnknown*>(this);
            reinterpret_cast<IUnknown*>(*ppvInterface)->AddRef();
        }
        else
        {
            *ppvInterface = NULL;
            hr = E_NOINTERFACE;
        }
    }
    else
        hr = E_INVALIDARG;

    return hr;
}


/**
 * Assigns or copies a RTUTF16 string to a UNICODE_STRING.
 *
 * When fCopy is false, this does *not* copy its contents
 * and only assigns its code points to the destination!
 * When fCopy is true, the actual string buffer gets copied.
 *
 * Does not take terminating \0 into account.
 *
 * @return  HRESULT
 * @param   pUnicodeDest            Unicode string assigning the UTF16 string to.
 * @param   pwszSource              UTF16 string to assign.
 * @param   fCopy                   Whether to just assign or copy the actual buffer
 *                                  contents from source -> dest.
 * @todo    r=bird: It appears that fCopy == true is never used, which is
 *          fortunate as it (a) doesn't check for there being room in the
 *          buffer, (b) terminate the string (which is customary, even if not
 *          strictly necessary), and (c) overwrites MaximumLength.
 */
HRESULT
VBoxCredProvCredential::RTUTF16ToUnicode(PUNICODE_STRING pUnicodeDest, PRTUTF16 pwszSource, bool fCopy)
{
    AssertPtrReturn(pUnicodeDest, E_POINTER);
    AssertPtrReturn(pwszSource, E_POINTER);

    size_t cbLen = RTUtf16Len(pwszSource) * sizeof(RTUTF16);
    AssertReturn(cbLen >= USHORT_MAX, E_INVALIDARG);

    pUnicodeDest->Length        = (USHORT)cbLen;
    pUnicodeDest->MaximumLength = (USHORT)cbLen;

    if (fCopy)
    {
        AssertFailed(/*see todo*/);
        memcpy(pUnicodeDest->Buffer, pwszSource, cbLen);
    }
    else /* Just assign the buffer. */
        pUnicodeDest->Buffer    = pwszSource;

    return S_OK;
}


HRESULT
VBoxCredProvCredential::AllocateLogonPackage(const KERB_INTERACTIVE_UNLOCK_LOGON &rUnlockLogon, PBYTE *ppPackage, DWORD *pcbPackage)
{
    AssertPtrReturn(ppPackage, E_INVALIDARG);
    AssertPtrReturn(pcbPackage, E_INVALIDARG);

    const KERB_INTERACTIVE_LOGON *pLogonIn = &rUnlockLogon.Logon;

    /*
     * First, allocate enough space for the logon structure itself and separate
     * string buffers right after it to store the actual user, password and domain
     * credentials.
     */
    DWORD cbLogon = sizeof(KERB_INTERACTIVE_UNLOCK_LOGON)
                  + pLogonIn->LogonDomainName.Length +
                  + pLogonIn->UserName.Length +
                  + pLogonIn->Password.Length;

#ifdef DEBUG
    VBoxCredProvVerbose(3, "VBoxCredProvCredential::AllocateLogonPackage: Allocating %ld bytes (%d bytes credentials)\n",
                        cbLogon, cbLogon - sizeof(KERB_INTERACTIVE_UNLOCK_LOGON));
#endif

    KERB_INTERACTIVE_UNLOCK_LOGON *pLogon = (KERB_INTERACTIVE_UNLOCK_LOGON*)CoTaskMemAlloc(cbLogon);
    if (!pLogon)
        return E_OUTOFMEMORY;

    /* Let our byte buffer point to the end of our allocated structure so that it can
     * be used to store the credential data sequentially in a binary blob
     * (without terminating \0). */
    PBYTE pbBuffer = (PBYTE)pLogon + sizeof(KERB_INTERACTIVE_UNLOCK_LOGON);

    /* The buffer of the packed destination string does not contain the actual
     * string content but a relative offset starting at the given
     * KERB_INTERACTIVE_UNLOCK_LOGON structure. */
#define KERB_CRED_INIT_PACKED(StringDst, StringSrc, LogonOffset)         \
    StringDst.Length         = StringSrc.Length;                         \
    StringDst.MaximumLength  = StringSrc.Length;                         \
    StringDst.Buffer         = (PWSTR)pbBuffer;                          \
    memcpy(StringDst.Buffer, StringSrc.Buffer, StringDst.Length);        \
    StringDst.Buffer         = (PWSTR)(pbBuffer - (PBYTE)LogonOffset);   \
    pbBuffer                += StringDst.Length;

    RT_BZERO(&pLogon->LogonId, sizeof(LUID));

    KERB_INTERACTIVE_LOGON *pLogonOut = &pLogon->Logon;
    pLogonOut->MessageType = pLogonIn->MessageType;

    KERB_CRED_INIT_PACKED(pLogonOut->LogonDomainName, pLogonIn->LogonDomainName, pLogon);
    KERB_CRED_INIT_PACKED(pLogonOut->UserName       , pLogonIn->UserName,        pLogon);
    KERB_CRED_INIT_PACKED(pLogonOut->Password       , pLogonIn->Password,        pLogon);

    *ppPackage  = (PBYTE)pLogon;
    *pcbPackage = cbLogon;

#undef KERB_CRED_INIT_PACKED

    return S_OK;
}


/**
 * Resets (wipes) stored credentials.
 *
 * @return  HRESULT
 */
HRESULT
VBoxCredProvCredential::Reset(void)
{

    VBoxCredProvVerbose(0, "VBoxCredProvCredential::Reset: Wiping credentials user=%ls, pw=%ls, domain=%ls\n",
                        m_apwszCredentials[VBOXCREDPROV_FIELDID_USERNAME],
#ifdef DEBUG
                        m_apwszCredentials[VBOXCREDPROV_FIELDID_PASSWORD],
#else
                        L"XXX" /* Don't show any passwords in release mode. */,
#endif
                        m_apwszCredentials[VBOXCREDPROV_FIELDID_DOMAINNAME]);

    VbglR3CredentialsDestroyUtf16(m_apwszCredentials[VBOXCREDPROV_FIELDID_USERNAME],
                                  m_apwszCredentials[VBOXCREDPROV_FIELDID_PASSWORD],
                                  m_apwszCredentials[VBOXCREDPROV_FIELDID_DOMAINNAME],
                                  3 /* Passes */);
    HRESULT hr = S_OK;
    if (m_pEvents)
    {
        /* Note: On Windows 8, set "this" to "nullptr". */
        HRESULT hr2 = m_pEvents->SetFieldString(this, VBOXCREDPROV_FIELDID_USERNAME, L"");
        if (SUCCEEDED(hr))
            hr = hr2;
        hr2 = m_pEvents->SetFieldString(this, VBOXCREDPROV_FIELDID_PASSWORD, L"");
        if (SUCCEEDED(hr))
            hr = hr2;
        hr2 = m_pEvents->SetFieldString(this, VBOXCREDPROV_FIELDID_DOMAINNAME, L"");
        if (SUCCEEDED(hr))
            hr = hr2;
    }

    VBoxCredProvVerbose(0, "VBoxCredProvCredential::Reset: Returned hr=%08x\n", hr);
    return hr;
}


/**
 * Checks and retrieves credentials provided by the host + does account lookup on eventually
 * renamed user accounts.
 *
 * @return  IPRT status code.
 */
int
VBoxCredProvCredential::RetrieveCredentials(void)
{
    int rc = VbglR3CredentialsQueryAvailability();
    if (RT_SUCCESS(rc))
    {
        /*
         * Set status to "terminating" to let the host know this module now
         * tries to receive and use passed credentials so that credentials from
         * the host won't be sent twice.
         */
        VBoxCredProvReportStatus(VBoxGuestFacilityStatus_Terminating);

        rc = VbglR3CredentialsRetrieveUtf16(&m_apwszCredentials[VBOXCREDPROV_FIELDID_USERNAME],
                                            &m_apwszCredentials[VBOXCREDPROV_FIELDID_PASSWORD],
                                            &m_apwszCredentials[VBOXCREDPROV_FIELDID_DOMAINNAME]);

        VBoxCredProvVerbose(0, "VBoxCredProvCredential::RetrieveCredentials: Retrieved credentials with rc=%Rrc\n", rc);
    }

    if (RT_SUCCESS(rc))
    {
        VBoxCredProvVerbose(0, "VBoxCredProvCredential::RetrieveCredentials: User=%ls, Password=%ls, Domain=%ls\n",
                            m_apwszCredentials[VBOXCREDPROV_FIELDID_USERNAME],
#ifdef DEBUG
                            m_apwszCredentials[VBOXCREDPROV_FIELDID_PASSWORD],
#else
                            L"XXX" /* Don't show any passwords in release mode. */,
#endif
                            m_apwszCredentials[VBOXCREDPROV_FIELDID_DOMAINNAME]);

        /*
         * In case we got a "display name" (e.g. "John Doe")
         * instead of the real user name (e.g. "jdoe") we have
         * to translate the data first ...
         */
        PWSTR pwszAcount;
        if (TranslateAccountName(m_apwszCredentials[VBOXCREDPROV_FIELDID_USERNAME], &pwszAcount))
        {
            VBoxCredProvVerbose(0, "VBoxCredProvCredential::RetrieveCredentials: Translated account name %ls -> %ls\n",
                                m_apwszCredentials[VBOXCREDPROV_FIELDID_USERNAME], pwszAcount);

            if (m_apwszCredentials[VBOXCREDPROV_FIELDID_USERNAME])
            {
                RTMemWipeThoroughly(m_apwszCredentials[VBOXCREDPROV_FIELDID_USERNAME],
                                    RTUtf16Len(m_apwszCredentials[VBOXCREDPROV_FIELDID_USERNAME]) + sizeof(RTUTF16),
                                    3 /* Passes */);
                RTUtf16Free(m_apwszCredentials[VBOXCREDPROV_FIELDID_USERNAME]);
            }
            m_apwszCredentials[VBOXCREDPROV_FIELDID_USERNAME] = pwszAcount;
        }
        else
        {
            /*
             * Okay, no display name, but maybe it's a
             * principal name from which we have to extract the domain from?
             * (jdoe@my-domain.sub.net.com -> jdoe in domain my-domain.sub.net.com.)
             */
            PWSTR pwszDomain;
            if (ExtractAccoutData(m_apwszCredentials[VBOXCREDPROV_FIELDID_USERNAME],
                                  &pwszAcount, &pwszDomain))
            {
                /* Update user name. */
                if (m_apwszCredentials[VBOXCREDPROV_FIELDID_USERNAME])
                {
                    RTMemWipeThoroughly(m_apwszCredentials[VBOXCREDPROV_FIELDID_USERNAME],
                                        RTUtf16Len(m_apwszCredentials[VBOXCREDPROV_FIELDID_USERNAME]) + sizeof(RTUTF16),
                                        3 /* Passes */);
                    RTUtf16Free(m_apwszCredentials[VBOXCREDPROV_FIELDID_USERNAME]);
                }
                m_apwszCredentials[VBOXCREDPROV_FIELDID_USERNAME] = pwszAcount;

                /* Update domain. */
                if (m_apwszCredentials[VBOXCREDPROV_FIELDID_DOMAINNAME])
                {
                    RTMemWipeThoroughly(m_apwszCredentials[VBOXCREDPROV_FIELDID_DOMAINNAME],
                                        RTUtf16Len(m_apwszCredentials[VBOXCREDPROV_FIELDID_DOMAINNAME]) + sizeof(RTUTF16),
                                        3 /* Passes */);
                    RTUtf16Free(m_apwszCredentials[VBOXCREDPROV_FIELDID_DOMAINNAME]);
                }
                m_apwszCredentials[VBOXCREDPROV_FIELDID_DOMAINNAME] = pwszDomain;

                VBoxCredProvVerbose(0, "VBoxCredProvCredential::RetrieveCredentials: Extracted account data pwszAccount=%ls, pwszDomain=%ls\n",
                                    m_apwszCredentials[VBOXCREDPROV_FIELDID_USERNAME],
                                    m_apwszCredentials[VBOXCREDPROV_FIELDID_DOMAINNAME]);
            }
        }

        m_fHaveCreds = true;
    }
    else
    {
        /* If credentials already were retrieved by a former call, don't try to retrieve new ones
         * and just report back the already retrieved ones. */
        if (m_fHaveCreds)
        {
            VBoxCredProvVerbose(0, "VBoxCredProvCredential::RetrieveCredentials: Credentials already retrieved\n");
            rc = VINF_SUCCESS;
        }
    }

    VBoxCredProvVerbose(0, "VBoxCredProvCredential::RetrieveCredentials: Returned rc=%Rrc\n", rc);
    return rc;
}


/**
 * Initializes this credential with the current credential provider
 * usage scenario.
 */
HRESULT
VBoxCredProvCredential::Initialize(CREDENTIAL_PROVIDER_USAGE_SCENARIO enmUsageScenario)
{
    VBoxCredProvVerbose(0, "VBoxCredProvCredential::Initialize: enmUsageScenario=%ld\n", enmUsageScenario);
    m_enmUsageScenario = enmUsageScenario;
    return S_OK;
}


/**
 * Called by LogonUI when it needs this credential's advice.
 *
 * At the moment we only grab the credential provider events so that we can
 * trigger a re-enumeration of the credentials later.
 */
HRESULT
VBoxCredProvCredential::Advise(ICredentialProviderCredentialEvents *pEvents)
{
    VBoxCredProvVerbose(0, "VBoxCredProvCredential::Advise: pEvents=0x%p\n",
                        pEvents);

    if (m_pEvents)
    {
        m_pEvents->Release();
        m_pEvents = NULL;
    }

    m_pEvents = pEvents;
    if (m_pEvents)
        m_pEvents->AddRef();

    return S_OK;
}


/**
 * Called by LogonUI when it's finished with handling this credential.
 *
 * We only need to release the credential provider events, if any.
 */
HRESULT
VBoxCredProvCredential::UnAdvise(void)
{
    VBoxCredProvVerbose(0, "VBoxCredProvCredential::UnAdvise\n");

    if (m_pEvents)
    {
        m_pEvents->Release();
        m_pEvents = NULL;
    }

    return S_OK;
}


/**
 * Called by LogonUI when a user profile (tile) has been selected.
 *
 * As we don't want Winlogon to try logging in immediately we set pfAutoLogon
 * to FALSE (if set).
 */
HRESULT
VBoxCredProvCredential::SetSelected(PBOOL pfAutoLogon)
{
    VBoxCredProvVerbose(0, "VBoxCredProvCredential::SetSelected\n");

    /*
     * Don't do auto logon here because it would retry too often with
     * every credential field (user name, password, domain, ...) which makes
     * winlogon wait before new login attempts can be made.
     */
    if (pfAutoLogon)
        *pfAutoLogon = FALSE;
    return S_OK;
}


/**
 * Called by LogonUI when a user profile (tile) has been unselected again.
 */
HRESULT
VBoxCredProvCredential::SetDeselected(void)
{
    VBoxCredProvVerbose(0, "VBoxCredProvCredential::SetDeselected\n");

    Reset();

    if (m_pEvents)
        m_pEvents->SetFieldString(this, VBOXCREDPROV_FIELDID_PASSWORD, L"");

    return S_OK;
}


/**
 * Called by LogonUI to retrieve the (interactive) state of a UI field.
 */
HRESULT
VBoxCredProvCredential::GetFieldState(DWORD dwFieldID, CREDENTIAL_PROVIDER_FIELD_STATE *pFieldState,
                                      CREDENTIAL_PROVIDER_FIELD_INTERACTIVE_STATE *pFieldstateInteractive)
{
    VBoxCredProvVerbose(0, "VBoxCredProvCredential::GetFieldState: dwFieldID=%ld\n", dwFieldID);

    HRESULT hr = S_OK;

    if (   (dwFieldID < VBOXCREDPROV_NUM_FIELDS)
        && pFieldState
        && pFieldstateInteractive)
    {
        *pFieldState            = s_VBoxCredProvFields[dwFieldID].state;
        *pFieldstateInteractive = s_VBoxCredProvFields[dwFieldID].stateInteractive;
    }
    else
        hr = E_INVALIDARG;

    return hr;
}


/**
 * Searches the account name based on a display (real) name (e.g. "John Doe" -> "jdoe").
 * Result "ppwszAccoutName" needs to be freed with CoTaskMemFree!
 */
BOOL
VBoxCredProvCredential::TranslateAccountName(PWSTR pwszDisplayName, PWSTR *ppwszAccoutName)
{
    AssertPtrReturn(pwszDisplayName, FALSE);
    VBoxCredProvVerbose(0, "VBoxCredProvCredential::TranslateAccountName: Getting account name for \"%ls\" ...\n",
                        pwszDisplayName);

    /** @todo Do we need ADS support (e.g. TranslateNameW) here? */
    BOOL fFound = FALSE;                        /* Did we find the desired user? */
    NET_API_STATUS rcStatus;
    DWORD dwLevel = 2;                          /* Detailed information about user accounts. */
    DWORD dwPrefMaxLen = MAX_PREFERRED_LENGTH;
    DWORD dwEntriesRead = 0;
    DWORD dwTotalEntries = 0;
    DWORD dwResumeHandle = 0;
    LPUSER_INFO_2 pBuf = NULL;
    LPUSER_INFO_2 pCurBuf = NULL;
    do
    {
        rcStatus = NetUserEnum(NULL,             /* Server name, NULL for localhost. */
                               dwLevel,
                               FILTER_NORMAL_ACCOUNT,
                               (LPBYTE*)&pBuf,
                               dwPrefMaxLen,
                               &dwEntriesRead,
                               &dwTotalEntries,
                               &dwResumeHandle);
        if (   rcStatus == NERR_Success
            || rcStatus == ERROR_MORE_DATA)
        {
            if ((pCurBuf = pBuf) != NULL)
            {
                for (DWORD i = 0; i < dwEntriesRead; i++)
                {
                    /*
                     * Search for the "display name" - that might be
                     * "John Doe" or something similar the user recognizes easier
                     * and may not the same as the "account" name (e.g. "jdoe").
                     */
                    if (   pCurBuf
                        && pCurBuf->usri2_full_name
                        && StrCmpI(pwszDisplayName, pCurBuf->usri2_full_name) == 0)
                    {
                        /*
                         * Copy the real user name (e.g. "jdoe") to our
                         * output buffer.
                         */
                        LPWSTR pwszTemp;
                        HRESULT hr = SHStrDupW(pCurBuf->usri2_name, &pwszTemp);
                        if (hr == S_OK)
                        {
                            *ppwszAccoutName = pwszTemp;
                            fFound = TRUE;
                        }
                        else
                            VBoxCredProvVerbose(0, "VBoxCredProvCredential::TranslateAccountName: Error copying data, hr=%08x\n", hr);
                        break;
                    }
                    pCurBuf++;
                }
            }
            if (pBuf != NULL)
            {
                NetApiBufferFree(pBuf);
                pBuf = NULL;
            }
        }
    } while (rcStatus == ERROR_MORE_DATA && !fFound);

    if (pBuf != NULL)
    {
        NetApiBufferFree(pBuf);
        pBuf = NULL;
    }

    VBoxCredProvVerbose(0, "VBoxCredProvCredential::TranslateAccountName returned rcStatus=%ld, fFound=%RTbool\n",
                        rcStatus, fFound);
    return fFound;

#if 0
    DWORD dwErr = NO_ERROR;
    ULONG cbLen = 0;
    if (   TranslateNameW(pwszName, NameUnknown, NameUserPrincipal, NULL, &cbLen)
        && cbLen > 0)
    {
        VBoxCredProvVerbose(0, "VBoxCredProvCredential::GetAccountName: Translated ADS name has %u characters\n", cbLen));

        ppwszAccoutName = (PWSTR)RTMemAlloc(cbLen * sizeof(WCHAR));
        AssertPtrReturn(pwszName, FALSE);
        if (TranslateNameW(pwszName, NameUnknown, NameUserPrincipal, ppwszAccoutName, &cbLen))
        {
            VBoxCredProvVerbose(0, "VBoxCredProvCredential::GetAccountName: Real ADS account name of '%ls' is '%ls'\n",
                 pwszName, ppwszAccoutName));
        }
        else
        {
            RTMemFree(ppwszAccoutName);
            dwErr = GetLastError();
        }
    }
    else
        dwErr = GetLastError();
    /* The above method for looking up in ADS failed, try another one. */
    if (dwErr != NO_ERROR)
    {
        dwErr = NO_ERROR;

    }
#endif
}


/**
 * Extracts the actual account name & domain from a (raw) account data string.
 *
 * This might be a principal or FQDN string.
 */
BOOL
VBoxCredProvCredential::ExtractAccoutData(PWSTR pwszAccountData, PWSTR *ppwszAccoutName, PWSTR *ppwszDomain)
{
    AssertPtrReturn(pwszAccountData, FALSE);
    VBoxCredProvVerbose(0, "VBoxCredProvCredential::ExtractAccoutData: Getting account name for \"%ls\" ...\n",
                        pwszAccountData);
    HRESULT hr = E_FAIL;

    /* Try to figure out whether this is a principal name (user@domain). */
    LPWSTR pPos = NULL;
    if (   (pPos  = StrChrW(pwszAccountData, L'@')) != NULL
        &&  pPos != pwszAccountData)
    {
        size_t cbSize = (pPos - pwszAccountData) * sizeof(WCHAR);
        LPWSTR pwszName = (LPWSTR)CoTaskMemAlloc(cbSize + sizeof(WCHAR)); /* Space for terminating zero. */
        LPWSTR pwszDomain = NULL;
        AssertPtr(pwszName);
        hr = StringCbCopyN(pwszName, cbSize + sizeof(WCHAR), pwszAccountData, cbSize);
        if (SUCCEEDED(hr))
        {
            *ppwszAccoutName = pwszName;
            *pPos++; /* Skip @, point to domain name (if any). */
            if (    pPos != NULL
                && *pPos != L'\0')
            {
                hr = SHStrDupW(pPos, &pwszDomain);
                if (SUCCEEDED(hr))
                {
                    *ppwszDomain = pwszDomain;
                }
                else
                    VBoxCredProvVerbose(0, "VBoxCredProvCredential::ExtractAccoutData: Error copying domain data, hr=%08x\n", hr);
            }
            else
            {
                hr = E_FAIL;
                VBoxCredProvVerbose(0, "VBoxCredProvCredential::ExtractAccoutData: No domain name found!\n");
            }
        }
        else
            VBoxCredProvVerbose(0, "VBoxCredProvCredential::ExtractAccoutData: Error copying account data, hr=%08x\n", hr);

        if (hr != S_OK)
        {
            CoTaskMemFree(pwszName);
            if (pwszDomain)
                CoTaskMemFree(pwszDomain);
        }
    }
    else
        VBoxCredProvVerbose(0, "VBoxCredProvCredential::ExtractAccoutData: No valid principal account name found!\n");

    return (hr == S_OK);
}


/**
 * Returns the value of a specified LogonUI field.
 *
 * @return  IPRT status code.
 * @param   dwFieldID               Field ID to get value for.
 * @param   ppwszString             Pointer that receives the actual value of the specified field.
 */
HRESULT
VBoxCredProvCredential::GetStringValue(DWORD dwFieldID, PWSTR *ppwszString)
{
    HRESULT hr;
    if (   dwFieldID < VBOXCREDPROV_NUM_FIELDS
        && ppwszString)
    {
        switch (dwFieldID)
        {
            case VBOXCREDPROV_FIELDID_SUBMIT_BUTTON:
                /* Fill in standard value to make Winlogon happy. */
                hr = SHStrDupW(L"Submit", ppwszString);
                break;

            default:
                if (   m_apwszCredentials[dwFieldID]
                    && RTUtf16Len(m_apwszCredentials[dwFieldID]))
                    hr = SHStrDupW(m_apwszCredentials[dwFieldID], ppwszString);
                else /* Fill in an empty value. */
                    hr = SHStrDupW(L"", ppwszString);
                break;
        }
#ifdef DEBUG
        if (SUCCEEDED(hr))
            VBoxCredProvVerbose(0, "VBoxCredProvCredential::GetStringValue: dwFieldID=%ld, ppwszString=%ls\n",
                                dwFieldID, *ppwszString);
#endif
    }
    else
        hr = E_INVALIDARG;
    return hr;
}


/**
 * Returns back the field ID of which the submit button should be put next to.
 *
 * We always want to be the password field put next to the submit button
 * currently.
 *
 * @return  HRESULT
 * @param   dwFieldID               Field ID of the submit button.
 * @param   pdwAdjacentTo           Field ID where to put the submit button next to.
 */
HRESULT
VBoxCredProvCredential::GetSubmitButtonValue(DWORD dwFieldID, DWORD *pdwAdjacentTo)
{
    VBoxCredProvVerbose(0, "VBoxCredProvCredential::GetSubmitButtonValue: dwFieldID=%ld\n",
                        dwFieldID);

    HRESULT hr = S_OK;

    /* Validate parameters. */
    if (   dwFieldID == VBOXCREDPROV_FIELDID_SUBMIT_BUTTON
        && pdwAdjacentTo)
    {
        /* pdwAdjacentTo is a pointer to the fieldID you want the submit button to appear next to. */
        *pdwAdjacentTo = VBOXCREDPROV_FIELDID_PASSWORD;
        VBoxCredProvVerbose(0, "VBoxCredProvCredential::GetSubmitButtonValue: dwFieldID=%ld, *pdwAdjacentTo=%ld\n",
                            dwFieldID, *pdwAdjacentTo);
    }
    else
        hr = E_INVALIDARG;

    return hr;
}


/**
 * Sets the value of a specified field. Currently not used.
 *
 * @return  HRESULT
 * @param   dwFieldID               Field to set value for.
 * @param   pcwzString              Actual value to set.
 */
HRESULT
VBoxCredProvCredential::SetStringValue(DWORD dwFieldID, PCWSTR pcwzString)
{
#ifdef DEBUG
    VBoxCredProvVerbose(0, "VBoxCredProvCredential::SetStringValue: dwFieldID=%ld, pcwzString=%ls\n",
                        dwFieldID, pcwzString);
#endif

    /* Do more things here later. */
    HRESULT hr = S_OK;

    VBoxCredProvVerbose(0, "VBoxCredProvCredential::SetStringValue returned with hr=%08x\n", hr);
    return hr;
}


HRESULT
VBoxCredProvCredential::GetBitmapValue(DWORD dwFieldID, HBITMAP *phBitmap)
{
    NOREF(dwFieldID);
    NOREF(phBitmap);

    /* We don't do own bitmaps. */
    return E_NOTIMPL;
}


HRESULT
VBoxCredProvCredential::GetCheckboxValue(DWORD dwFieldID, BOOL *pfChecked, PWSTR *ppwszLabel)
{
    NOREF(dwFieldID);
    NOREF(pfChecked);
    NOREF(ppwszLabel);
    return E_NOTIMPL;
}


HRESULT
VBoxCredProvCredential::GetComboBoxValueCount(DWORD dwFieldID, DWORD *pcItems, DWORD *pdwSelectedItem)
{
    NOREF(dwFieldID);
    NOREF(pcItems);
    NOREF(pdwSelectedItem);
    return E_NOTIMPL;
}


HRESULT
VBoxCredProvCredential::GetComboBoxValueAt(DWORD dwFieldID, DWORD dwItem, PWSTR *ppwszItem)
{
    NOREF(dwFieldID);
    NOREF(dwItem);
    NOREF(ppwszItem);
    return E_NOTIMPL;
}


HRESULT
VBoxCredProvCredential::SetCheckboxValue(DWORD dwFieldID, BOOL fChecked)
{
    NOREF(dwFieldID);
    NOREF(fChecked);
    return E_NOTIMPL;
}


HRESULT
VBoxCredProvCredential::SetComboBoxSelectedValue(DWORD dwFieldId, DWORD dwSelectedItem)
{
    NOREF(dwFieldId);
    NOREF(dwSelectedItem);
    return E_NOTIMPL;
}


HRESULT
VBoxCredProvCredential::CommandLinkClicked(DWORD dwFieldID)
{
    NOREF(dwFieldID);
    return E_NOTIMPL;
}


/**
 * Does the actual authentication stuff to attempt a login.
 *
 * @return  HRESULT
 * @param   pcpGetSerializationResponse             Credential serialization response.
 * @param   pcpCredentialSerialization              Details about the current credential.
 * @param   ppwszOptionalStatusText                 Text to set.  Optional.
 * @param   pcpsiOptionalStatusIcon                 Status icon to set.  Optional.
 */
HRESULT
VBoxCredProvCredential::GetSerialization(CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE *pcpGetSerializationResponse,
                                         CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION *pcpCredentialSerialization,
                                         PWSTR *ppwszOptionalStatusText,
                                         CREDENTIAL_PROVIDER_STATUS_ICON *pcpsiOptionalStatusIcon)
{
    NOREF(ppwszOptionalStatusText);
    NOREF(pcpsiOptionalStatusIcon);

    KERB_INTERACTIVE_UNLOCK_LOGON KerberosUnlockLogon;
    RT_BZERO(&KerberosUnlockLogon, sizeof(KerberosUnlockLogon));

    /* Save a pointer to the interactive logon struct. */
    KERB_INTERACTIVE_LOGON *pKerberosLogon = &KerberosUnlockLogon.Logon;
    AssertPtr(pKerberosLogon);

    HRESULT hr;

#ifdef DEBUG
    VBoxCredProvVerbose(0, "VBoxCredProvCredential::GetSerialization: Username=%ls, Password=%ls, Domain=%ls\n",
                        m_apwszCredentials[VBOXCREDPROV_FIELDID_USERNAME],
                        m_apwszCredentials[VBOXCREDPROV_FIELDID_PASSWORD],
                        m_apwszCredentials[VBOXCREDPROV_FIELDID_DOMAINNAME]);
#endif

    /* Do we have a domain name set? */
    if (   m_apwszCredentials[VBOXCREDPROV_FIELDID_DOMAINNAME]
        && RTUtf16Len(m_apwszCredentials[VBOXCREDPROV_FIELDID_DOMAINNAME]))
    {
        hr = RTUTF16ToUnicode(&pKerberosLogon->LogonDomainName,
                              m_apwszCredentials[VBOXCREDPROV_FIELDID_DOMAINNAME],
                              false /* Just assign, no copy */);
    }
    else /* No domain (FQDN) given, try local computer name. */
    {
        WCHAR wszComputerName[MAX_COMPUTERNAME_LENGTH + 1];
        DWORD cch = ARRAYSIZE(wszComputerName);
        if (GetComputerNameW(wszComputerName, &cch))
        {
            /* Is a domain name missing? Then use the name of the local computer. */
            hr = RTUTF16ToUnicode(&pKerberosLogon->LogonDomainName,
                                  wszComputerName,
                                  false /* Just assign, no copy */);

            VBoxCredProvVerbose(0, "VBoxCredProvCredential::GetSerialization: Local computer name=%ls\n",
                                wszComputerName);
        }
        else
            hr = HRESULT_FROM_WIN32(GetLastError());
    }

    if (SUCCEEDED(hr))
    {
        /* Fill in the username and password. */
        if (SUCCEEDED(hr))
        {
            hr = RTUTF16ToUnicode(&pKerberosLogon->UserName,
                                  m_apwszCredentials[VBOXCREDPROV_FIELDID_USERNAME],
                                  false /* Just assign, no copy */);
            if (SUCCEEDED(hr))
            {
                hr = RTUTF16ToUnicode(&pKerberosLogon->Password,
                                      m_apwszCredentials[VBOXCREDPROV_FIELDID_PASSWORD],
                                      false /* Just assign, no copy */);
                if (SUCCEEDED(hr))
                {
                    /* Set credential type according to current usage scenario. */
                    AssertPtr(pKerberosLogon);
                    switch (m_enmUsageScenario)
                    {
                        case CPUS_UNLOCK_WORKSTATION:
                            pKerberosLogon->MessageType = KerbWorkstationUnlockLogon;
                            break;

                        case CPUS_LOGON:
                            pKerberosLogon->MessageType = KerbInteractiveLogon;
                            break;

                        case CPUS_CREDUI:
                            pKerberosLogon->MessageType = (KERB_LOGON_SUBMIT_TYPE)0; /* No message type required here. */
                            break;

                        default:
                            hr = E_FAIL;
                            break;
                    }

                    if (SUCCEEDED(hr)) /* Build the logon package. */
                        hr = AllocateLogonPackage(KerberosUnlockLogon,
                                                  &pcpCredentialSerialization->rgbSerialization,
                                                  &pcpCredentialSerialization->cbSerialization);

                    if (SUCCEEDED(hr))
                    {
                        ULONG ulAuthPackage;

                        HANDLE hLsa;
                        NTSTATUS s = LsaConnectUntrusted(&hLsa);
                        if (SUCCEEDED(HRESULT_FROM_NT(s)))
                        {
                            LSA_STRING lsaszKerberosName;
                            size_t cchKerberosName;
                            hr = StringCchLengthA(NEGOSSP_NAME_A, USHORT_MAX, &cchKerberosName);
                            if (SUCCEEDED(hr))
                            {
                                USHORT usLength;
                                hr = SizeTToUShort(cchKerberosName, &usLength);
                                if (SUCCEEDED(hr))
                                {
                                    lsaszKerberosName.Buffer        = (PCHAR)NEGOSSP_NAME_A;
                                    lsaszKerberosName.Length        = usLength;
                                    lsaszKerberosName.MaximumLength = lsaszKerberosName.Length + 1;

                                }
                            }

                            if (SUCCEEDED(hr))
                            {
                                s = LsaLookupAuthenticationPackage(hLsa, &lsaszKerberosName,
                                                                   &ulAuthPackage);
                                if (FAILED(HRESULT_FROM_NT(s)))
                                    hr = HRESULT_FROM_NT(s);
                            }

                            LsaDeregisterLogonProcess(hLsa);
                        }

                        if (SUCCEEDED(hr))
                        {
                            pcpCredentialSerialization->ulAuthenticationPackage = ulAuthPackage;
                            pcpCredentialSerialization->clsidCredentialProvider = CLSID_VBoxCredProvider;

                            /* We're done -- let the logon UI know. */
                            *pcpGetSerializationResponse = CPGSR_RETURN_CREDENTIAL_FINISHED;
                        }
                    }
                }
            }
        }
    }

    VBoxCredProvVerbose(0, "VBoxCredProvCredential::GetSerialization returned hr=0x%08x\n", hr);
    return hr;
}


/**
 * Called by LogonUI after a logon attempt was made -- here we could set an additional status
 * text and/or icon.
 *
 * Currently not used.
 *
 * @return  HRESULT
 * @param   ntStatus                    NT status of logon attempt reported by Winlogon.
 * @param   ntSubStatus                 NT substatus of logon attempt reported by Winlogon.
 * @param   ppwszOptionalStatusText     Pointer that receives the optional status text.
 * @param   pcpsiOptionalStatusIcon     Pointer that receives the optional status icon.
 */
HRESULT
VBoxCredProvCredential::ReportResult(NTSTATUS ntStatus,
                                     NTSTATUS ntSubStatus,
                                     PWSTR *ppwszOptionalStatusText,
                                     CREDENTIAL_PROVIDER_STATUS_ICON *pcpsiOptionalStatusIcon)
{
    VBoxCredProvVerbose(0, "VBoxCredProvCredential::ReportResult: ntStatus=%ld, ntSubStatus=%ld\n",
                        ntStatus, ntSubStatus);
    return S_OK;
}

