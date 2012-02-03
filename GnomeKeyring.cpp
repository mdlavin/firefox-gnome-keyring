/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is Gnome Keyring password manager storage.
 *
 * The Initial Developer of the Original Code is
 * Sylvain Pasche <sylvain.pasche@gmail.com>
 * Portions created by the Initial Developer are Copyright (C) 2007
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 * Matt Lavin <matt.lavin@gmail.com>
 * Luca Niccoli <lultimouomo@gmail.com>
 * Ximin Luo <infinity0@gmx.com>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#include "GnomeKeyring.h"
#include "nsMemory.h"
#include "nsILoginInfo.h"

#include "mozilla/ModuleUtils.h"
#include "nsComponentManagerUtils.h"
#include "nsStringAPI.h"
#include "nsIXULAppInfo.h"
#include "nsXULAppAPI.h"
#include "nsServiceManagerUtils.h"
#include "nsIPropertyBag.h"
#include "nsIProperty.h"
#include "nsIVariant.h"
#include "nsIPrefService.h"
#include "nsIPrefBranch.h"

#pragma GCC visibility push(default)
extern "C" {
#include "gnome-keyring.h"
}
#pragma GCC visibility pop

#ifdef PR_LOGGING
PRLogModuleInfo *gGnomeKeyringLog;
#endif

/**
 * Keyring name to save the password to. This is controlled by the preference
 * item "extensions.gnome-keyring.keyringName". The default is "mozilla".
 *
 * Note that password will be retrieved from every unlocked keyring,
 * because the gnome-keyring API doens't provide a way to search in
 * just one keyring.
 */
nsCString keyringName;

static const char *kPrefsBranch = "extensions.gnome-keyring.";
static const char *kPrefsKeyring = "keyringName";
static const char *kDefaultKeyring = "mozilla";

// TODO should use profile identifier instead of a constant
#define UNIQUE_PROFILE_ID "v1"

/** magic signature that a keyring item represents login information */
static const char *kLoginInfoMagicAttrName = "mozLoginInfoMagic";
static const char *kLoginInfoMagicAttrValue = "loginInfoMagic" UNIQUE_PROFILE_ID;

/** attribute names for a login information entry */
static const char *kHostnameAttr = "hostname";
static const char *kFormSubmitURLAttr = "formSubmitURL";
static const char *kHttpRealmAttr = "httpRealm";
static const char *kUsernameFieldAttr = "usernameField";
static const char *kPasswordFieldAttr = "passwordField";
static const char *kUsernameAttr = "username";
static const char *kPasswordAttr = "password";

/** magic signature that a keyring item represents disabled hostnames */
static const char *kDisabledHostMagicAttrName = "mozDisabledHostMagic";
static const char *kDisabledHostMagicAttrValue = "disabledHostMagic" UNIQUE_PROFILE_ID;

/** attribute names for a disabled hostname entry */
static const char *kDisabledHostAttrName = "disabledHost";

// Utility macros and data structures

// Macro to check result of a gnome-keyring method, and convert it to a
// mozilla return code. Note that find methods return a NO_MATCH code
// when no items are returned; if this should not be considered an
// error, use the CHECKF macro instead.
#define MGK_GK_CHECK_NS(x)                                    \
  PR_BEGIN_MACRO                                              \
    if (x != GNOME_KEYRING_RESULT_OK) {                       \
       NS_WARNING("MGK_GK_CHECK_NS(" #x ") failed");          \
       return NS_ERROR_FAILURE;                               \
    }                                                         \
  PR_END_MACRO

// Macro to check result of a gnome-keyring find method, and convert it
// to a mozilla return code. NO_MATCH is considered to be successful.
#define MGK_GK_CHECKF_NS(x)                                   \
  PR_BEGIN_MACRO                                              \
    if (x != GNOME_KEYRING_RESULT_OK &&                       \
        x != GNOME_KEYRING_RESULT_NO_MATCH) {                 \
       NS_WARNING("MGK_GK_CHECKF_NS(" #x ") failed");         \
       return NS_ERROR_FAILURE;                               \
    }                                                         \
  PR_END_MACRO

#define MGK_GK_CHECK(x, bail)                                 \
  PR_BEGIN_MACRO                                              \
    if (x != GNOME_KEYRING_RESULT_OK) {                       \
       NS_WARNING("MGK_GK_CHECK(" #x ") failed");             \
       goto bail;                                             \
    }                                                         \
  PR_END_MACRO

/**
 * Wrapper to automatically free a pointer to a complex data structure.
 */
template<class T, void F(T *)>
class AutoPtr {
  public:
    AutoPtr() : mPtr(nsnull) { }

    operator T*() {
      return mPtr;
    }
    T* operator->() {
      return mPtr;
    }
    T** operator&() {
      return &mPtr;
    }
    ~AutoPtr() {
      if (mPtr)
        F(mPtr);
    }
  private:
    T *mPtr;
};

/**
 * Deep free of GnomeKeyringFoundList, a GList of GnomeKeyringFound.
 */
typedef AutoPtr<GList, gnome_keyring_found_list_free> AutoFoundList;
/**
 * Deep free of GnomeKeyringAttributeList, a GArray of GnomeKeyringAttribute.
 */
typedef AutoPtr<GArray, gnome_keyring_attribute_list_free> AutoAttributeList;
/**
 * Deep free of GnomeKeyringItemInfo
 */
typedef AutoPtr<GnomeKeyringItemInfo, gnome_keyring_item_info_free> AutoItemInfo;
/**
 * Shallow free of a GList.
 */
typedef AutoPtr<GList, g_list_free> AutoIdList;

// Utilities

/**
 * Filter out items based on a user-supplied function and data. Code
 * adapted from GLib's g_list_remove_all.
 *
 * @filter: gpointer -> bool. false to remove from the array. should
 *          also free the memory used by the data, if appropriate.
 */
GList*
_g_list_remove_all_custom(GList* list,
                          gboolean (*filter)(gconstpointer, gpointer),
                          gconstpointer user_data)
{
  GList *tmp = list;

  while (tmp)
    {
      if (filter(user_data, tmp->data))
      tmp = tmp->next;
      else
      {
        GList *next = tmp->next;

        if (tmp->prev)
          tmp->prev->next = next;
        else
          list = next;
        if (next)
          next->prev = tmp->prev;

          g_slice_free(GList, tmp);
        tmp = next;
      }
    }
  return list;
}

gboolean
matchKeyring(gconstpointer user_data, gpointer data)
{
  GnomeKeyringFound* found = static_cast<GnomeKeyringFound*>(data);
  const char* keyring = static_cast<const char*>(user_data);
  gboolean keep = !strcmp(found->keyring, keyring);
  if (!keep) {
    gnome_keyring_found_free(found);
  }
  return keep;
}

void
addAttribute(GnomeKeyringAttributeList* attributes,
             const char* name,
             const nsAString & value)
{
  gnome_keyring_attribute_list_append_string(
          attributes, name, NS_ConvertUTF16toUTF8(value).get());
}

template<class T>
void
copyAttribute(GnomeKeyringAttributeList* attributes,
              T* source,
              nsresult (T::*getAttr)(nsAString&),
              const char* name)
{
  nsAutoString value;
  (source->*getAttr)(value);
  addAttribute(attributes, name, value);
}

template<class T>
void
copyAttributeOr(GnomeKeyringAttributeList* attributes,
                T* source,
                nsresult (T::*getAttr)(nsAString&),
                const char* name)
{
  nsAutoString value;
  (source->*getAttr)(value);
  if (!value.IsVoid()) {
    addAttribute(attributes, name, value);
  }
}

/** name is currently ignored and assumed to be kPasswordAttr */
template<class T>
void
setSecret(GnomeKeyringItemInfo* itemInfo,
          T* source,
          nsresult (T::*getAttr)(nsAString&),
          const char* name)
{
  nsAutoString value;
  (source->*getAttr)(value);
  gnome_keyring_item_info_set_secret(itemInfo,
                                     NS_ConvertUTF16toUTF8(value).get());
}

template<class T>
void
withBagItem(nsIPropertyBag* source,
            T* target,
            void (*dothis)(T*,
                           nsIVariant*,
                           nsresult (nsIVariant::*)(nsAString&),
                           const char*),
            const char* name)
{
  nsAutoString property;
  nsCOMPtr<nsIVariant> propValue;

  property.AssignLiteral(name);
  nsresult result = source->GetProperty(property, getter_AddRefs(propValue));
  if ( result == NS_ERROR_FAILURE ) {
    return;
  }
  dothis(target, propValue.get(), &nsIVariant::GetAsAString, name);
}

void
newLoginInfoAttributes(GnomeKeyringAttributeList** attributes)
{
  *attributes = gnome_keyring_attribute_list_new();
  gnome_keyring_attribute_list_append_string(
          *attributes, kLoginInfoMagicAttrName, kLoginInfoMagicAttrValue);
}

void
newDisabledHostsAttributes(GnomeKeyringAttributeList** attributes)
{
  *attributes = gnome_keyring_attribute_list_new();
  gnome_keyring_attribute_list_append_string(
          *attributes, kDisabledHostMagicAttrName, kDisabledHostMagicAttrValue);
}

void
appendAttributesFromLogin(nsILoginInfo *aLogin,
                          GnomeKeyringAttributeList* attrList)
{
  typedef nsILoginInfo L;
  copyAttribute(attrList, aLogin, &L::GetHostname, kHostnameAttr);
  copyAttribute(attrList, aLogin, &L::GetUsername, kUsernameAttr);
  copyAttribute(attrList, aLogin, &L::GetUsernameField, kUsernameFieldAttr);
  copyAttribute(attrList, aLogin, &L::GetPasswordField, kPasswordFieldAttr);
  // formSubmitURL and httpRealm are not guaranteed to be set.
  copyAttributeOr(attrList, aLogin, &L::GetFormSubmitURL, kFormSubmitURLAttr);
  copyAttributeOr(attrList, aLogin, &L::GetHttpRealm, kHttpRealmAttr);
}

/** attributes must already have loginInfo magic set */
void
appendAttributesFromBag(nsIPropertyBag *matchData,
                        GnomeKeyringAttributeList* attrList)
{
  withBagItem(matchData, attrList, copyAttribute, kHostnameAttr);
  withBagItem(matchData, attrList, copyAttribute, kUsernameAttr);
  withBagItem(matchData, attrList, copyAttribute, kUsernameFieldAttr);
  withBagItem(matchData, attrList, copyAttribute, kPasswordFieldAttr);
  // formSubmitURL and httpRealm are not guaranteed to be set.
  withBagItem(matchData, attrList, copyAttributeOr, kFormSubmitURLAttr);
  withBagItem(matchData, attrList, copyAttributeOr, kHttpRealmAttr);
}

void
appendItemInfoFromBag(nsIPropertyBag *matchData,
                      GnomeKeyringItemInfo* itemInfo)
{
  withBagItem(matchData, itemInfo, setSecret, kPasswordAttr);
}

nsresult
GnomeKeyring::deleteFoundItems(GList* foundList,
                               bool aExpectOnlyOne = PR_FALSE)
{
  if (foundList == NULL) {
    GK_LOG(("Found not items to delete"));
    return NS_OK;
  }

  bool deleted = PR_FALSE;
  for (GList* l = foundList; l != NULL; l = l->next, deleted = PR_TRUE)
  {
    if (aExpectOnlyOne && deleted)
      NS_WARNING("Expected only one item to delete, but found more");

    GnomeKeyringFound* found = static_cast<GnomeKeyringFound*>(l->data);
    GK_LOG(("Found item with id %i\n", found->item_id));

    GnomeKeyringResult result = gnome_keyring_item_delete_sync(found->keyring,
                                                               found->item_id);
    MGK_GK_CHECK_NS(result);
  }
  return NS_OK;
}

GnomeKeyringResult
GnomeKeyring::findItems(GnomeKeyringItemType type,
                        GnomeKeyringAttributeList* attributes,
                        GList** found)
{
  GnomeKeyringResult result;
  result = gnome_keyring_find_items_sync(type, attributes, found);

  MGK_GK_CHECK(result, bail1);
  *found = _g_list_remove_all_custom(*found, matchKeyring, keyringName.get());
  bail1:
  return result;
}

GnomeKeyringResult
GnomeKeyring::findLoginItems(GnomeKeyringAttributeList* attributes,
                             GList** found)
{
  return findItems(GNOME_KEYRING_ITEM_GENERIC_SECRET, attributes, found);
}

GnomeKeyringResult
GnomeKeyring::findLogins(const nsAString & aHostname,
                         const nsAString & aActionURL,
                         const nsAString & aHttpRealm,
                         GList** found)
{
  AutoAttributeList attributes;
  newLoginInfoAttributes(&attributes);

  addAttribute(attributes, kHostnameAttr, aHostname);
  if (!aActionURL.IsVoid() && !aActionURL.IsEmpty()) {
    addAttribute(attributes, kFormSubmitURLAttr, aActionURL);
  }
  if (!aHttpRealm.IsVoid() && !aHttpRealm.IsEmpty()) {
    addAttribute(attributes, kHttpRealmAttr, aHttpRealm);
  }

  GnomeKeyringResult result = findLoginItems(attributes, found);

  if (aActionURL.IsVoid()) {
    // TODO: filter out items with a kFormSubmitURLAttr
    // practically, this is not necessary since actionURL/httpRealm are
    // mutually exclusive, but this is not enforced by the interface
  }
  if (aHttpRealm.IsVoid()) {
    // TODO: filter out items with a kHttpRealmAttr
    // practically, this is not necessary since actionURL/httpRealm are
    // mutually exclusive, but this is not enforced by the interface
  }

  return result;
}

GnomeKeyringResult
GnomeKeyring::findHostItemsAll(GList** found)
{
  AutoAttributeList attributes;
  newDisabledHostsAttributes(&attributes);
  return findItems(GNOME_KEYRING_ITEM_NOTE, attributes, found);
}

GnomeKeyringResult
GnomeKeyring::findHostItems(const nsAString & aHost,
                            GList** found)
{
  AutoAttributeList attributes;
  newDisabledHostsAttributes(&attributes);
  addAttribute(attributes, kDisabledHostAttrName, aHost);
  // TODO: expect only one
  return findItems(GNOME_KEYRING_ITEM_NOTE, attributes, found);
}

nsILoginInfo*
foundToLoginInfo(GnomeKeyringFound* found)
{
  nsCOMPtr<nsILoginInfo> loginInfo = do_CreateInstance(NS_LOGININFO_CONTRACTID);
  if (!loginInfo)
    return nsnull;

  loginInfo->SetPassword(NS_ConvertUTF8toUTF16(found->secret));

  GnomeKeyringAttribute *attrArray =
    (GnomeKeyringAttribute *)found->attributes->data;

  for (PRUint32 i = 0; i < found->attributes->len; i++) {

    if (attrArray[i].type != GNOME_KEYRING_ATTRIBUTE_TYPE_STRING)
      continue;

    const char *attrName = attrArray[i].name;
    const char *attrValue = attrArray[i].value.string;
    GK_LOG(("Attr value %s\n", attrValue));

    if (!strcmp(attrName, kHostnameAttr))
     loginInfo->SetHostname(NS_ConvertUTF8toUTF16(attrValue));
    else if (!strcmp(attrName, kFormSubmitURLAttr))
     loginInfo->SetFormSubmitURL(NS_ConvertUTF8toUTF16(attrValue));
    else if (!strcmp(attrName, kHttpRealmAttr))
     loginInfo->SetHttpRealm(NS_ConvertUTF8toUTF16(attrValue));
    else if (!strcmp(attrName, kUsernameAttr))
     loginInfo->SetUsername(NS_ConvertUTF8toUTF16(attrValue));
    else if (!strcmp(attrName, kUsernameFieldAttr))
     loginInfo->SetUsernameField(NS_ConvertUTF8toUTF16(attrValue));
    else if (!strcmp(attrName, kPasswordFieldAttr))
     loginInfo->SetPasswordField(NS_ConvertUTF8toUTF16(attrValue));
    else
      NS_WARNING(("Unknown %s attribute name", attrName));
  }
  NS_ADDREF((nsILoginInfo*) loginInfo);
  return loginInfo;
}

PRUnichar *
foundToHost(GnomeKeyringFound* found)
{
  PRUnichar *host = NULL;

  GnomeKeyringAttribute *attrArray =
    (GnomeKeyringAttribute *)found->attributes->data;

  for (PRUint32 i = 0; i < found->attributes->len; i++) {

    if (attrArray[i].type != GNOME_KEYRING_ATTRIBUTE_TYPE_STRING)
      continue;

    const char *attrName = attrArray[i].name;
    const char *attrValue = attrArray[i].value.string;
    GK_LOG(("Attr value %s\n", attrValue));

    if (!strcmp(attrName, kDisabledHostAttrName))
      host = NS_StringCloneData(NS_ConvertUTF8toUTF16(attrValue));
  }

  // TODO what to do in that case?
  if (!host)
    host = NS_StringCloneData(NS_ConvertASCIItoUTF16("undefined"));

  return host;
}

template<class T>
nsresult
foundListToArray(T (*aFoundToObject)(GnomeKeyringFound*),
                 GList* aFoundList, PRUint32 *aCount, T **aArray)
{
  PRUint32 count = 0;
  GList *l = aFoundList;
  while (l) {
    count++;
    l = l->next;
  }
  GK_LOG(("Num items: %i\n", count));

  T *array = static_cast<T*>(nsMemory::Alloc(count * sizeof(T)));
  NS_ENSURE_TRUE(array, NS_ERROR_OUT_OF_MEMORY);

  memset(array, 0, count * sizeof(T));

  PRUint32 i = 0;
  for (GList* l = aFoundList; l != NULL; l = l->next, i++) {
    GnomeKeyringFound* found = static_cast<GnomeKeyringFound*>(l->data);
    GK_LOG(("Found item with id %i\n", found->item_id));

    T obj = aFoundToObject(found);
    NS_ENSURE_STATE(obj);
    array[i] = obj;
  }

  *aCount = count;
  *aArray = array;
  return NS_OK;
}

/* Implementation file */

// see https://developer.mozilla.org/En/NsILoginManagerStorage
// see nsILoginManagerStorage.h

/// The following code works around the problem that newILoginManagerStorage has a new UUID in
/// Firefox 4.0, but this component should be compatible with both 3.6 and 4.0.

#define LOGIN_MANAGER_STORAGE_3_6_CID {0xe66c97cd, 0x3bcf, 0x4eee, { 0x99, 0x37, 0x38, 0xf6, 0x50, 0x37, 0x2d, 0x77 }}
NS_DEFINE_NAMED_CID(LOGIN_MANAGER_STORAGE_3_6_CID);

NS_IMPL_ADDREF(GnomeKeyring)
NS_IMPL_RELEASE(GnomeKeyring)

NS_INTERFACE_MAP_BEGIN(GnomeKeyring)
NS_INTERFACE_MAP_ENTRY(nsILoginManagerStorage)
  if ( aIID.Equals(kLOGIN_MANAGER_STORAGE_3_6_CID ) )
    foundInterface = static_cast<nsILoginManagerStorage*>(this);
  else
NS_INTERFACE_MAP_ENTRY_AMBIGUOUS(nsISupports, nsILoginManagerStorage)
NS_INTERFACE_MAP_END

// End code to deal with 4.0  / 3.6 compatibility

NS_IMETHODIMP GnomeKeyring::Init()
{
  nsresult ret;
  nsCOMPtr<nsIServiceManager> servMan;
  nsCOMPtr<nsIPrefService> prefService;
  nsCOMPtr<nsIPrefBranch> pref;
#ifdef PR_LOGGING
  gGnomeKeyringLog = PR_NewLogModule("GnomeKeyringLog");
#endif
  keyringName.AssignLiteral(kDefaultKeyring);
  ret = NS_GetServiceManager(getter_AddRefs(servMan));
  if (ret != NS_OK) { return ret; }

  ret = servMan->GetServiceByContractID("@mozilla.org/preferences-service;1",
                                        NS_GET_IID(nsIPrefService),
                                        getter_AddRefs(prefService));
  if (ret != NS_OK) { return ret; }

  ret = prefService->ReadUserPrefs(nsnull);
  if (ret != NS_OK) { return ret; }

  ret = prefService->GetBranch(kPrefsBranch, getter_AddRefs(pref));
  if (ret != NS_OK) { return ret; }

  PRInt32 prefType;
  ret = pref->GetPrefType(kPrefsKeyring, &prefType);
  if (ret != NS_OK) { return ret; }

  if (prefType == 32) {
    char* tempKeyringName;
    pref->GetCharPref(kPrefsKeyring, &tempKeyringName);
    keyringName = tempKeyringName;
    if ( keyringName.IsVoid() ) keyringName.AssignLiteral(kDefaultKeyring);
  }

  /* Create the password keyring, it doesn't hurt if it already exists */
  GnomeKeyringResult result = gnome_keyring_create_sync(keyringName.get(), NULL);
  if ((result != GNOME_KEYRING_RESULT_OK) &&
      (result != GNOME_KEYRING_RESULT_KEYRING_ALREADY_EXISTS)) {
    NS_ERROR("Can't open or create password keyring!");
    return NS_ERROR_FAILURE;
  }
  return ret;
}

NS_IMETHODIMP GnomeKeyring::InitWithFile(nsIFile *aInputFile,
                                         nsIFile *aOutputFile)
{
  // TODO
  return Init();
}

NS_IMETHODIMP GnomeKeyring::AddLogin(nsILoginInfo *aLogin)
{
  AutoAttributeList attributes;
  newLoginInfoAttributes(&attributes);
  appendAttributesFromLogin(aLogin, attributes);

  nsAutoString password, hostname;
  aLogin->GetPassword(password);
  aLogin->GetHostname(hostname);
  guint itemId;

  GnomeKeyringResult result = gnome_keyring_item_create_sync(keyringName.get(),
                                        GNOME_KEYRING_ITEM_GENERIC_SECRET,
                                        NS_ConvertUTF16toUTF8(hostname).get(),
                                        attributes,
                                        NS_ConvertUTF16toUTF8(password).get(),
                                        TRUE,
                                        &itemId);
  MGK_GK_CHECK_NS(result);

  return NS_OK;
}

NS_IMETHODIMP GnomeKeyring::RemoveLogin(nsILoginInfo *aLogin)
{
  AutoAttributeList attributes;
  newLoginInfoAttributes(&attributes);
  appendAttributesFromLogin(aLogin, attributes);

  AutoFoundList foundList;
  GnomeKeyringResult result = findLoginItems(attributes, &foundList);
  MGK_GK_CHECK_NS(result);

  return deleteFoundItems(foundList, PR_TRUE);
}

NS_IMETHODIMP GnomeKeyring::ModifyLogin(nsILoginInfo *oldLogin,
                                        nsISupports *modLogin)
{
  nsresult interfaceok;

  /* If the second argument is an nsILoginInfo,
   * just remove the old login and add the new one */
  nsCOMPtr<nsILoginInfo> newLogin( do_QueryInterface(modLogin, &interfaceok) );
  if (interfaceok == NS_OK) {
    nsresult rv = RemoveLogin(oldLogin);
    rv |= AddLogin(newLogin);
    return rv;
  }

  /* Otherwise, it has to be an nsIPropertyBag.
   * Let's get the attributes from the old login, then append the ones
   * fetched from the property bag. Gracefully, if an attribute appears
   * twice in an attribut list, the last value is stored. */
  nsCOMPtr<nsIPropertyBag> matchData( do_QueryInterface(modLogin, &interfaceok) );
  if (interfaceok != NS_OK) {
    return interfaceok;
  }

  GnomeKeyringResult result;

  AutoAttributeList attributes;
  newLoginInfoAttributes(&attributes);
  appendAttributesFromLogin(oldLogin, attributes);

  // We need the id of the keyring item to set its attributes.
  AutoFoundList foundList;
  result = findLoginItems(attributes, &foundList);
  MGK_GK_CHECK_NS(result);
  PRUint32 i = 0, id;
  for (GList* l = foundList; l != NULL; l = l->next, i++)
  {
    GnomeKeyringFound* found = static_cast<GnomeKeyringFound*>(l->data);
    id = found->item_id;
    if (i >= 1) {
      NS_ERROR("found more than one item to edit");
      return NS_ERROR_FAILURE;
    }
  }

  // set new attributes
  appendAttributesFromBag(matchData.get(), attributes);
  result = gnome_keyring_item_set_attributes_sync(keyringName.get(),
                                                  id,
                                                  attributes);
  MGK_GK_CHECK_NS(result);

  // set new iteminfo, e.g. password
  AutoItemInfo itemInfo;
  result = gnome_keyring_item_get_info_sync(keyringName.get(),
                                            id,
                                            &itemInfo);
  MGK_GK_CHECK_NS(result);
  appendItemInfoFromBag(matchData.get(), itemInfo);
  result = gnome_keyring_item_set_info_sync(keyringName.get(),
                                            id,
                                            itemInfo);
  MGK_GK_CHECK_NS(result);

  return NS_OK;
}

NS_IMETHODIMP GnomeKeyring::RemoveAllLogins()
{
  AutoAttributeList attributes;
  newLoginInfoAttributes(&attributes);

  AutoFoundList foundList;
  GnomeKeyringResult result = findLoginItems(attributes, &foundList);
  MGK_GK_CHECKF_NS(result);

  return deleteFoundItems(foundList);
}

NS_IMETHODIMP GnomeKeyring::GetAllLogins(PRUint32 *aCount,
                                         nsILoginInfo ***aLogins)
{
  AutoAttributeList attributes;
  newLoginInfoAttributes(&attributes);

  AutoFoundList foundList;
  GnomeKeyringResult result = findLoginItems(attributes, &foundList);
  MGK_GK_CHECKF_NS(result);

  return foundListToArray(foundToLoginInfo, foundList, aCount, aLogins);
}

NS_IMETHODIMP GnomeKeyring::GetAllEncryptedLogins(unsigned int*,
                                                  nsILoginInfo***)
{
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP GnomeKeyring::SearchLogins(PRUint32 *count,
                                         nsIPropertyBag *matchData,
                                         nsILoginInfo ***logins)
{
  AutoAttributeList attributes;
  newLoginInfoAttributes(&attributes);
  appendAttributesFromBag(matchData, attributes);

  AutoFoundList foundList;
  GnomeKeyringResult result = findLoginItems(attributes, &foundList);
  MGK_GK_CHECKF_NS(result);

  return foundListToArray(foundToLoginInfo, foundList, count, logins);
}

NS_IMETHODIMP GnomeKeyring::GetAllDisabledHosts(PRUint32 *aCount,
                                                PRUnichar ***aHostnames)
{
  AutoFoundList foundList;
  GnomeKeyringResult result = findHostItemsAll(&foundList);
  MGK_GK_CHECKF_NS(result);

  return foundListToArray(foundToHost, foundList, aCount, aHostnames);
}

NS_IMETHODIMP GnomeKeyring::GetLoginSavingEnabled(const nsAString & aHost,
                                                  bool *_retval)
{
  AutoFoundList foundList;
  GnomeKeyringResult result = findHostItems(aHost, &foundList);
  MGK_GK_CHECKF_NS(result);

  *_retval = foundList == NULL;
  return NS_OK;
}

NS_IMETHODIMP GnomeKeyring::SetLoginSavingEnabled(const nsAString & aHost,
                                                  bool isEnabled)
{
  GnomeKeyringResult result;

  if (isEnabled) {
    AutoFoundList foundList;
    result = findHostItems(aHost, &foundList);
    MGK_GK_CHECKF_NS(result);

    return deleteFoundItems(foundList, PR_TRUE);
  }

  // TODO should check if host is already enabled

  AutoAttributeList attributes;
  newDisabledHostsAttributes(&attributes);
  addAttribute(attributes, kDisabledHostAttrName, aHost);

  // TODO name should be more explicit
  const char* name = "Mozilla disabled host entry";
  guint itemId;

  result = gnome_keyring_item_create_sync(keyringName.get(),
            GNOME_KEYRING_ITEM_NOTE,
            name,
            attributes,
            "", // no secret
            TRUE,
            &itemId);

  MGK_GK_CHECK_NS(result);
  return NS_OK;
}

NS_IMETHODIMP GnomeKeyring::FindLogins(PRUint32 *count,
                                       const nsAString & aHostname,
                                       const nsAString & aActionURL,
                                       const nsAString & aHttpRealm,
                                       nsILoginInfo ***logins)
{
  AutoFoundList allFound;
  GnomeKeyringResult result = findLogins(aHostname,
                                         aActionURL,
                                         aHttpRealm,
                                         &allFound);

  MGK_GK_CHECKF_NS(result);

  return foundListToArray(foundToLoginInfo, allFound, count, logins);
}

NS_IMETHODIMP GnomeKeyring::CountLogins(const nsAString & aHostname,
                                        const nsAString & aActionURL,
                                        const nsAString & aHttpRealm,
                                        PRUint32 *_retval)
{
  AutoFoundList allFound;
  GnomeKeyringResult result = findLogins(aHostname,
                                         aActionURL,
                                         aHttpRealm,
                                         &allFound);

  MGK_GK_CHECKF_NS(result);

  *_retval = g_list_length(allFound);
  return NS_OK;
}

/**
 * True when a master password prompt is being shown.
 */
/* readonly attribute boolean uiBusy; */
NS_IMETHODIMP GnomeKeyring::GetUiBusy(bool *aUiBusy)
{
  *aUiBusy = FALSE;
  return NS_OK;
}



/* End of implementation class template. */

NS_GENERIC_FACTORY_CONSTRUCTOR(GnomeKeyring)

NS_DEFINE_NAMED_CID(GNOMEKEYRING_CID);

// Build a table of ClassIDs (CIDs) which are implemented by this module. CIDs
// should be completely unique UUIDs.
// each entry has the form { CID, service, factoryproc, constructorproc }
// where factoryproc is usually NULL.
static const mozilla::Module::CIDEntry kPasswordsCIDs[] = {
    { &kGNOMEKEYRING_CID, false, NULL, GnomeKeyringConstructor },
    { NULL }
};

// Build a table which maps contract IDs to CIDs.
// A contract is a string which identifies a particular set of functionality. In some
// cases an extension component may override the contract ID of a builtin gecko component
// to modify or extend functionality.
static const mozilla::Module::ContractIDEntry kPasswordsContracts[] = {
    { GNOMEPASSWORD_CONTRACTID, &kGNOMEKEYRING_CID },
    { NULL }
};

// Category entries are category/key/value triples which can be used
// to register contract ID as content handlers or to observe certain
// notifications. Most modules do not need to register any category
// entries: this is just a sample of how you'd do it.
// @see nsICategoryManager for information on retrieving category data.
static const mozilla::Module::CategoryEntry kPasswordsCategories[] = {
    { "login-manager-storage", "nsILoginManagerStorage", GNOMEPASSWORD_CONTRACTID },
    { NULL }
};

static const mozilla::Module kPasswordsModule = {
    mozilla::Module::kVersion,
    kPasswordsCIDs,
    kPasswordsContracts,
    kPasswordsCategories
};


// The following line implements the one-and-only "NSModule" symbol exported from this
// shared library.
NSMODULE_DEFN(nsGnomeKeypassModule) = &kPasswordsModule;

// The following line implements the one-and-only "NSGetModule" symbol
// for compatibility with mozilla 1.9.2. You should only use this
// if you need a binary which is backwards-compatible and if you use
// interfaces carefully across multiple versions.
NS_IMPL_MOZILLA192_NSGETMODULE(&kPasswordsModule)
