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
#include "nsILoginInfo.h"

#include "nsIGenericFactory.h"
#include "nsMemory.h"
#include "nsICategoryManager.h"
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
/* create the preference item extensions.gnome-keyring.keyringName
 * to set wich keyring save the password to. The default is mozilla.
 * Note that password will be retrieved from every unlocked keyring,
 * because the gnome-keyring API doens't provide a way to search in
 * just one keyring.
 */
nsCString keyringName;

// XXX should use profile identifier instead of a constant
#define UNIQUE_PROFILE_ID "v1"

const char *kLoginInfoMagicAttrName = "mozLoginInfoMagic";
const char *kLoginInfoMagicAttrValue = "loginInfoMagic" UNIQUE_PROFILE_ID;

// For hostnames:
const char *kDisabledHostMagicAttrName = "mozDisabledHostMagic";
const char *kDisabledHostMagicAttrValue = "disabledHostMagic" UNIQUE_PROFILE_ID;

const char *kDisabledHostAttrName = "disabledHost";

const char *kHostnameAttr = "hostname";
const char *kFormSubmitURLAttr = "formSubmitURL";
const char *kHttpRealmAttr = "httpRealm";
const char *kUsernameFieldAttr = "usernameField";
const char *kPasswordFieldAttr = "passwordField";
const char *kUsernameAttr = "username";
const char *kPasswordAttr = "password";

// Macro to check gnome-keyring results
#define GK_ENSURE_SUCCESS(x)                                  \
  PR_BEGIN_MACRO                                              \
    if (x != GNOME_KEYRING_RESULT_OK) {                       \
       NS_WARNING("GK_ENSURE_SUCCESS(" #x ") failed");        \
       return NS_ERROR_FAILURE;                               \
    }                                                         \
  PR_END_MACRO

// A bug in gnome-keyring makes it return DENIED in case nothing is found
// See http://bugzilla.gnome.org/show_bug.cgi?id=447315
// This is fixed in gnome-keyring trunk
#define GK_ENSURE_SUCCESS_BUGGY(x)                            \
  PR_BEGIN_MACRO                                              \
    if (x != GNOME_KEYRING_RESULT_OK &&                       \
        x != GNOME_KEYRING_RESULT_NO_MATCH) {		      \
       NS_WARNING("GK_ENSURE_SUCCESS_BUGGY(" #x ") failed");  \
       return NS_ERROR_FAILURE;                               \
    }                                                         \
  PR_END_MACRO

// Wrapper to automatically free the found list when going out of scope
class AutoFoundList {
  public:
    AutoFoundList() : mFoundList(nsnull) { }

    operator GList*() {
      return mFoundList;
    }
    GList* operator->() {
      return mFoundList;
    }
    GList** operator&() {
      return &mFoundList;
    }
    ~AutoFoundList() {
      if (mFoundList)
        gnome_keyring_found_list_free(mFoundList);
    }
  private:
    GList *mFoundList;
};

// Utilities

GnomeKeyringAttributeList *
GnomeKeyring::buildAttributeList(nsILoginInfo *aLogin)
{
  nsAutoString s;
  GnomeKeyringAttributeList *attributes = gnome_keyring_attribute_list_new();

  aLogin->GetHostname(s);
  const char* tempHostname = NS_ConvertUTF16toUTF8(s).get();
  gnome_keyring_attribute_list_append_string(attributes, kHostnameAttr,
                                             tempHostname);
  
//  formSubmitURL and httpRealm are not guaranteed to be set.

  aLogin->GetFormSubmitURL(s);
  if (!s.IsVoid()) {
    const char* tempActionUrl = NS_ConvertUTF16toUTF8(s).get();
    gnome_keyring_attribute_list_append_string(attributes, kFormSubmitURLAttr,
					       tempActionUrl);
  }

  aLogin->GetHttpRealm(s);
  if (!s.IsVoid()) {
    const char* tempHttpRealm = NS_ConvertUTF16toUTF8(s).get();
    gnome_keyring_attribute_list_append_string(attributes, kHttpRealmAttr,
					       tempHttpRealm);
  }

  aLogin->GetUsername(s);
  gnome_keyring_attribute_list_append_string(attributes, kUsernameAttr,
                                             NS_ConvertUTF16toUTF8(s).get());
  aLogin->GetUsernameField(s);
  gnome_keyring_attribute_list_append_string(attributes, kUsernameFieldAttr,
                                             NS_ConvertUTF16toUTF8(s).get());
  aLogin->GetPasswordField(s);
  gnome_keyring_attribute_list_append_string(attributes, kPasswordFieldAttr,
                                             NS_ConvertUTF16toUTF8(s).get());

  gnome_keyring_attribute_list_append_string(attributes,
                                             kLoginInfoMagicAttrName,
                                             kLoginInfoMagicAttrValue);

  return attributes;
}

void GnomeKeyring::appendAttributesFromBag(nsIPropertyBag *matchData,
                                    GnomeKeyringAttributeList * &attributes)
{
  nsAutoString s, property, propName;
  nsCOMPtr<nsIVariant> propValue;
  nsresult result;

  gnome_keyring_attribute_list_append_string(attributes,
                                             kLoginInfoMagicAttrName,
                                             kLoginInfoMagicAttrValue);
  property.AssignLiteral(kHostnameAttr);
  result = matchData->GetProperty(property, getter_AddRefs(propValue));
  if ( result != NS_ERROR_FAILURE ) {
    propValue->GetAsAString(s);
    const char* tempValue = NS_ConvertUTF16toUTF8(s).get();
    gnome_keyring_attribute_list_append_string(attributes,
                                               kHostnameAttr,
                                               tempValue);
  }

//  formSubmitURL and httpRealm are not guaranteed to be set.

  property.AssignLiteral(kFormSubmitURLAttr);
  result = matchData->GetProperty(property, getter_AddRefs(propValue));
  if ( result != NS_ERROR_FAILURE ) {
    propValue->GetAsAString(s);
    if (!s.IsVoid()){
      const char* tempValue = NS_ConvertUTF16toUTF8(s).get();
      gnome_keyring_attribute_list_append_string(attributes,
                                                 kFormSubmitURLAttr,
                                                 tempValue);
    }
  }

  property.AssignLiteral(kHttpRealmAttr);
  result = matchData->GetProperty(property, getter_AddRefs(propValue));
  if ( result != NS_ERROR_FAILURE ) {
    propValue->GetAsAString(s);
    if (!s.IsVoid()){
      const char* tempValue = NS_ConvertUTF16toUTF8(s).get();
      gnome_keyring_attribute_list_append_string(attributes,
                                               kHttpRealmAttr,
                                               tempValue);
    }
  }
                                      
  property.AssignLiteral(kUsernameFieldAttr);
  result = matchData->GetProperty(property, getter_AddRefs(propValue));
  if ( result != NS_ERROR_FAILURE ) {
    propValue->GetAsAString(s);
    const char* tempValue = NS_ConvertUTF16toUTF8(s).get();
    gnome_keyring_attribute_list_append_string(attributes,
                                               kUsernameFieldAttr,
                                               tempValue);
  }
    
  property.AssignLiteral(kPasswordFieldAttr);
  result = matchData->GetProperty(property, getter_AddRefs(propValue));
  if ( result != NS_ERROR_FAILURE ) {
    propValue->GetAsAString(s);
    const char* tempValue = NS_ConvertUTF16toUTF8(s).get();
    gnome_keyring_attribute_list_append_string(attributes,
                                               kPasswordFieldAttr,
                                               tempValue);
  }
  
  property.AssignLiteral(kUsernameAttr);
  result = matchData->GetProperty(property, getter_AddRefs(propValue));
  if ( result != NS_ERROR_FAILURE ) {
    propValue->GetAsAString(s);
    const char* tempValue = NS_ConvertUTF16toUTF8(s).get();
    gnome_keyring_attribute_list_append_string(attributes,
                                               kUsernameAttr,
                                               tempValue);
    }

}

nsresult GnomeKeyring::deleteFoundItems(GList* foundList,
                                 PRBool aExpectOnlyOne = PR_FALSE)
{
  if (foundList == NULL) {
    GK_LOG(("Found not items to delete"));
    return NS_OK;
  }

  PRUint32 i = 0;
  for (GList* l = foundList; l != NULL; l = l->next, i++)
  {
    GnomeKeyringFound* found = static_cast<GnomeKeyringFound*>(l->data);
    GK_LOG(("Found item with id %i\n", found->item_id));

    GnomeKeyringResult result = gnome_keyring_item_delete_sync(keyringName.get(),
                                                               found->item_id);
    if (result != GNOME_KEYRING_RESULT_OK) {
      return NS_ERROR_FAILURE;
    }

    if (i == 1 && aExpectOnlyOne)
      NS_WARNING("Expected only one item to delete, but found more");
  }
  return NS_OK;
}

nsILoginInfo* 
loginToLogin(nsILoginInfo* found)
{
  return found;
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
  NS_ADDREF(loginInfo);
  return loginInfo;
}

PRUnichar *
foundToHost(GnomeKeyringFound* found)
{
  PRUnichar *host=NULL;

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

  // XXX what to do in that case?
  if (!host)
    host = NS_StringCloneData(NS_ConvertASCIItoUTF16("undefined"));

  return host;
}

template<class T, class T2>
nsresult foundListToArray(T (*aFoundToObject)(T2 found),
                          GList *aFoundList, PRUint32 *aCount, T **aArray)
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
    T2 found = static_cast<T2>(l->data);
    GK_LOG(("Found item with id %i\n", found->item_id));

    T obj = aFoundToObject(found);
    NS_ENSURE_STATE(obj);
    array[i] = obj;
  }

  *aCount = count;
  *aArray = array;
  return NS_OK;
}

void
convertAndCollectLogins(GnomeKeyringFound* found, GList **aFoundList)
{
  nsILoginInfo* info = foundToLoginInfo(found);
  *aFoundList = g_list_append(*aFoundList, info);
}

void
countFoundLogins(GnomeKeyringFound* found, int* count)
{
  (*count) = (*count)+1;
}

void
checkAttribute(const char* valuePattern, const char* value, bool* isMatch)
{
  if (valuePattern == NULL) {
    *isMatch = FALSE;
  } else {
    // If the valuePattern is "" then it should match
    // with everything
    if (!strcmp("", valuePattern)) {
      return;
    }
    
    if (strcmp(valuePattern, value)) {
      *isMatch = FALSE;
    }
  }
}

template<class T> 
GnomeKeyringResult
findLogins(const nsAString & aHostname,
           const nsAString & aActionURL,
           const nsAString & aHttpRealm,
	   void (*foundLogin)(GnomeKeyringFound* found, T data),
	   T data)
{
  GnomeKeyringAttributeList *attributes = gnome_keyring_attribute_list_new();

  gnome_keyring_attribute_list_append_string(attributes,
                          kLoginInfoMagicAttrName, kLoginInfoMagicAttrValue);
  const char* tempHostname = NS_ConvertUTF16toUTF8(aHostname).get();
  gnome_keyring_attribute_list_append_string(attributes, kHostnameAttr,
					     tempHostname);
  
  GList* unfiltered;
  GnomeKeyringResult result = gnome_keyring_find_items_sync(
                                        GNOME_KEYRING_ITEM_GENERIC_SECRET,
                                        attributes,
                                        &unfiltered );

  gnome_keyring_attribute_list_free(attributes);

  const char* tempActionUrl = aActionURL.IsVoid() ? NULL : NS_ConvertUTF16toUTF8(aActionURL).get();
  const char* tempHttpRealm = aHttpRealm.IsVoid() ? NULL : NS_ConvertUTF16toUTF8(aHttpRealm).get();

  for (GList* l = unfiltered; l != NULL; l = l->next) {
    bool isMatch = TRUE;
    GnomeKeyringFound* found = static_cast<GnomeKeyringFound*>(l->data);

    GnomeKeyringAttribute *attrArray = (GnomeKeyringAttribute *)found->attributes->data;

    for (PRUint32 i = 0; i < found->attributes->len; i++) {
      if (attrArray[i].type != GNOME_KEYRING_ATTRIBUTE_TYPE_STRING)
	continue;

      const char *attrName = attrArray[i].name;
      const char *attrValue = attrArray[i].value.string;

      if (!strcmp(attrName, kFormSubmitURLAttr)) {
	checkAttribute(tempActionUrl, attrValue, &isMatch);
      } else
      if (!strcmp(attrName, kHttpRealmAttr)) {
	checkAttribute(tempHttpRealm, attrValue, &isMatch);
      }
    }

    if (isMatch) {
      foundLogin(found, data);
    }
  }

  gnome_keyring_found_list_free(unfiltered);

  return result;
}

/* Implementation file */
NS_IMPL_ISUPPORTS1(GnomeKeyring, nsILoginManagerStorage)

NS_IMETHODIMP GnomeKeyring::Init()
{
  nsresult ret;
  nsCOMPtr<nsIServiceManager> servMan; 
  nsCOMPtr<nsIPrefService> prefService;
  nsCOMPtr<nsIPrefBranch> pref;
#ifdef PR_LOGGING
  gGnomeKeyringLog = PR_NewLogModule("GnomeKeyringLog");
#endif
  keyringName.AssignLiteral("mozilla");
  ret = NS_GetServiceManager(getter_AddRefs(servMan));
  if (ret == NS_OK) {
    ret = servMan->
      GetServiceByContractID("@mozilla.org/preferences-service;1",
                             NS_GET_IID(nsIPrefService),
                             getter_AddRefs(prefService));
    if (ret == NS_OK) {
    ret = prefService->ReadUserPrefs(nsnull);
      if (ret == NS_OK) {
        ret = prefService->
          GetBranch("extensions.gnome-keyring.", getter_AddRefs(pref));
        if (ret == NS_OK) {
          PRInt32 prefType;
          ret = pref->GetPrefType("keyringName", &prefType);
          if ((ret == NS_OK) && (prefType == 32)) {
            char* tempKeyringName;
            pref->GetCharPref("keyringName", &tempKeyringName);
            keyringName = tempKeyringName;
            if ( keyringName.IsVoid() ) keyringName.AssignLiteral("mozilla");
          }
        }
      }
    }
  } 

/* Create the password keyring, it doesn't hurt if it already exists */
  GnomeKeyringResult result = gnome_keyring_create_sync(keyringName.get(), NULL);
  if ((result != GNOME_KEYRING_RESULT_OK) &&
     (result != GNOME_KEYRING_RESULT_ALREADY_EXISTS)) {
    ret = NS_ERROR_FAILURE;
    NS_ERROR("Can't open or create password keyring!");
  }
  return ret;
}

NS_IMETHODIMP GnomeKeyring::InitWithFile(nsIFile *aInputFile,
                                         nsIFile *aOutputFile)
{
    return Init();
}

NS_IMETHODIMP GnomeKeyring::AddLogin(nsILoginInfo *aLogin)
{
  GnomeKeyringAttributeList *attributes = buildAttributeList(aLogin);

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
  gnome_keyring_attribute_list_free(attributes);
  GK_ENSURE_SUCCESS(result);

  return NS_OK;
}

NS_IMETHODIMP GnomeKeyring::RemoveLogin(nsILoginInfo *aLogin)
{
  GnomeKeyringAttributeList *attributes = buildAttributeList(aLogin);
  AutoFoundList foundList;

  GnomeKeyringResult result = gnome_keyring_find_items_sync(
                                GNOME_KEYRING_ITEM_GENERIC_SECRET,
                                attributes, &foundList);
  gnome_keyring_attribute_list_free(attributes);

  if (result != GNOME_KEYRING_RESULT_OK)
    return NS_ERROR_FAILURE;

  return deleteFoundItems(foundList, PR_TRUE);
}

NS_IMETHODIMP GnomeKeyring::ModifyLogin(nsILoginInfo *oldLogin,
                                        nsISupports *modLogin)
{
  /* If the second argument is an nsILoginInfo, 
   * just remove the old login and add the new one */

  nsresult interfaceok;
  nsCOMPtr<nsILoginInfo> newLogin( do_QueryInterface(modLogin, &interfaceok) );
  if (interfaceok == NS_OK) {
    nsresult rv = RemoveLogin(oldLogin);
    rv |= AddLogin(newLogin);
  return rv;
  } /* Otherwise, it has to be an nsIPropertyBag.
     * Let's get the attributes from the old login, then append the ones 
     * fetched from the property bag. Gracefully, if an attribute appears
     * twice in an attribut list, the last value is stored. */
    else {
    nsCOMPtr<nsIPropertyBag> matchData( do_QueryInterface(modLogin, &interfaceok) );
    if (interfaceok == NS_OK) {
      GnomeKeyringAttributeList *attributes = buildAttributeList(oldLogin);
      AutoFoundList foundList;
      
      GnomeKeyringResult result = gnome_keyring_find_items_sync(
                                  GNOME_KEYRING_ITEM_GENERIC_SECRET,
                                  attributes, &foundList);

      if (result != GNOME_KEYRING_RESULT_OK) {
          return NS_ERROR_FAILURE;
      }

      if (foundList == NULL) {
        return NS_ERROR_FAILURE;
      }

      appendAttributesFromBag(static_cast<nsIPropertyBag*>(matchData), attributes);

      // We need the id of the keyring item to set its attributes.
 
      PRUint32 i = 0, id;
      for (GList* l = foundList; l != NULL; l = l->next, i++)
      {
        GnomeKeyringFound* found = static_cast<GnomeKeyringFound*>(l->data);
        id = found->item_id; 
        if (i >= 1){
          return NS_ERROR_FAILURE;
        }
      }
      result = gnome_keyring_item_set_attributes_sync(keyringName.get(),
                                                      id,
                                                      attributes);
      gnome_keyring_attribute_list_free(attributes);
      if (result != GNOME_KEYRING_RESULT_OK) {
        return NS_ERROR_FAILURE; }
      return NS_OK;
    } else return interfaceok;
  }
}
 

NS_IMETHODIMP GnomeKeyring::RemoveAllLogins()
{
  AutoFoundList foundList;

  GnomeKeyringResult result = gnome_keyring_find_itemsv_sync(
                GNOME_KEYRING_ITEM_GENERIC_SECRET,
                &foundList,
                kLoginInfoMagicAttrName, GNOME_KEYRING_ATTRIBUTE_TYPE_STRING,
                kLoginInfoMagicAttrValue,
                NULL);

  GK_ENSURE_SUCCESS_BUGGY(result);

  return deleteFoundItems(foundList);
}

NS_IMETHODIMP GnomeKeyring::GetAllLogins(PRUint32 *aCount,
                                         nsILoginInfo ***aLogins)
{
  AutoFoundList foundList;

  GnomeKeyringResult result = gnome_keyring_find_itemsv_sync(
                                GNOME_KEYRING_ITEM_GENERIC_SECRET,
                                &foundList,
                                kLoginInfoMagicAttrName,
                                GNOME_KEYRING_ATTRIBUTE_TYPE_STRING,
                                kLoginInfoMagicAttrValue,
                                NULL);

  GK_ENSURE_SUCCESS_BUGGY(result);

  return foundListToArray(foundToLoginInfo, foundList, aCount, aLogins);
}

NS_IMETHODIMP GnomeKeyring::FindLogins(PRUint32 *count,
                                       const nsAString & aHostname,
                                       const nsAString & aActionURL,
                                       const nsAString & aHttpRealm,
                                       nsILoginInfo ***logins)
{

  GList* allFound = NULL;

  GnomeKeyringResult result = findLogins(aHostname,
					 aActionURL,
					 aHttpRealm,
					 convertAndCollectLogins,
					 &allFound);

  GK_ENSURE_SUCCESS_BUGGY(result);

  return foundListToArray(loginToLogin, allFound, count, logins);
}

NS_IMETHODIMP GnomeKeyring::SearchLogins(PRUint32 *count,
                                         nsIPropertyBag *matchData,
                                         nsILoginInfo ***logins)
{
  AutoFoundList foundList;
  GnomeKeyringAttributeList *attributes = gnome_keyring_attribute_list_new();
  appendAttributesFromBag(matchData, attributes);
  
  GnomeKeyringResult result = gnome_keyring_find_items_sync(
                                        GNOME_KEYRING_ITEM_GENERIC_SECRET,
                                        attributes,
                                        &foundList );
  GK_ENSURE_SUCCESS_BUGGY(result);
  gnome_keyring_attribute_list_free(attributes);
  return foundListToArray(foundToLoginInfo, foundList, count, logins); 

}
NS_IMETHODIMP GnomeKeyring::GetAllEncryptedLogins(unsigned int*,
                                                  nsILoginInfo***)
{
  return NS_ERROR_NOT_IMPLEMENTED;
}
NS_IMETHODIMP GnomeKeyring::GetAllDisabledHosts(PRUint32 *aCount,
                                                PRUnichar ***aHostnames)
{
  AutoFoundList foundList;

  GnomeKeyringResult result = gnome_keyring_find_itemsv_sync(
          GNOME_KEYRING_ITEM_NOTE,
          &foundList,
          kDisabledHostMagicAttrName, GNOME_KEYRING_ATTRIBUTE_TYPE_STRING,
          kDisabledHostMagicAttrValue,
          NULL);

  GK_ENSURE_SUCCESS_BUGGY(result);

  return foundListToArray(foundToHost, foundList, aCount, aHostnames);
}

NS_IMETHODIMP GnomeKeyring::GetLoginSavingEnabled(const nsAString & aHost,
                                                  PRBool *_retval)
{
  AutoFoundList foundList;

  GnomeKeyringResult result = gnome_keyring_find_itemsv_sync(
          GNOME_KEYRING_ITEM_NOTE,
          &foundList,
          kDisabledHostMagicAttrName, GNOME_KEYRING_ATTRIBUTE_TYPE_STRING,
          kDisabledHostMagicAttrValue,
          kDisabledHostAttrName, GNOME_KEYRING_ATTRIBUTE_TYPE_STRING,
          NS_ConvertUTF16toUTF8(aHost).get(),
          NULL);

  GK_ENSURE_SUCCESS_BUGGY(result);

  *_retval = foundList == NULL;
  return NS_OK;
}

NS_IMETHODIMP GnomeKeyring::SetLoginSavingEnabled(const nsAString & aHost,
                                                  PRBool isEnabled)
{
  GnomeKeyringResult result;

  if (isEnabled) {
    AutoFoundList foundList;

    result = gnome_keyring_find_itemsv_sync(
              GNOME_KEYRING_ITEM_NOTE,
              &foundList,
              kDisabledHostMagicAttrName, GNOME_KEYRING_ATTRIBUTE_TYPE_STRING,
              kDisabledHostMagicAttrValue,
              kDisabledHostAttrName, GNOME_KEYRING_ATTRIBUTE_TYPE_STRING,
              NS_ConvertUTF16toUTF8(aHost).get(),
              NULL);

    GK_ENSURE_SUCCESS_BUGGY(result);
    return deleteFoundItems(foundList, PR_TRUE);
  }

  // XXX should check if host is already enabled

  GnomeKeyringAttributeList *attributes;

  attributes = gnome_keyring_attribute_list_new();
  gnome_keyring_attribute_list_append_string(attributes,
            kDisabledHostMagicAttrName, kDisabledHostMagicAttrValue);
  gnome_keyring_attribute_list_append_string(attributes,
            kDisabledHostAttrName, NS_ConvertUTF16toUTF8(aHost).get());

  // XXX name should be more explicit
  const char* name = "Mozilla disabled host entry";
  guint itemId;

  result = gnome_keyring_item_create_sync(keyringName.get(),
            GNOME_KEYRING_ITEM_NOTE,
            name,
            attributes,
            "", // no secret
            TRUE,
            &itemId);
  gnome_keyring_attribute_list_free (attributes);

  GK_ENSURE_SUCCESS(result);
  return NS_OK;
}

NS_IMETHODIMP GnomeKeyring::CountLogins(const nsAString & aHostname, 
                                        const nsAString & aActionURL,
                                        const nsAString & aHttpRealm,
                                        PRUint32 *_retval)
{
  GnomeKeyringResult result;
  int count=0;

  result = findLogins(aHostname,
		      aActionURL,
		      aHttpRealm,
		      countFoundLogins,
		      &count);

  GK_ENSURE_SUCCESS_BUGGY(result);

  *_retval = count;
  return NS_OK;
}


/* End of implementation class template. */

static NS_METHOD
GnomeKeyringRegisterSelf(nsIComponentManager *compMgr, nsIFile *path,
                         const char *loaderStr, const char *type,
                         const nsModuleComponentInfo *info)
{
  nsCOMPtr<nsICategoryManager> cat =
      do_GetService(NS_CATEGORYMANAGER_CONTRACTID);
  NS_ENSURE_STATE(cat);

  cat->AddCategoryEntry("login-manager-storage", "nsILoginManagerStorage",
                        kGnomeKeyringContractID, PR_TRUE, PR_TRUE, nsnull);
  return NS_OK;
}

static NS_METHOD
GnomeKeyringUnregisterSelf(nsIComponentManager *compMgr, nsIFile *path,
                           const char *loaderStr,
                           const nsModuleComponentInfo *info)
{
  nsCOMPtr<nsICategoryManager> cat =
      do_GetService(NS_CATEGORYMANAGER_CONTRACTID);
  NS_ENSURE_STATE(cat);

  cat->DeleteCategoryEntry("login-manager-storage", "nsILoginManagerStorage",
                           PR_TRUE);
  return NS_OK;
}

NS_GENERIC_FACTORY_CONSTRUCTOR(GnomeKeyring)

static const nsModuleComponentInfo components[] = {
  {
    "GnomeKeyring",
    GNOMEKEYRING_CID,
    kGnomeKeyringContractID,
    GnomeKeyringConstructor,
    GnomeKeyringRegisterSelf,
    GnomeKeyringUnregisterSelf
  }
};

NS_IMPL_NSGETMODULE(GnomeKeyring, components)
