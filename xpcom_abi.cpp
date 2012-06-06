#include <stdio.h>

#include "nsIXULRuntime.h"
#include "nsServiceManagerUtils.h"
#include "nsStringAPI.h"
#include "nsXPCOM.h"
#include "nsXPCOMCIDInternal.h"

/**
 * Print the XPCOM ABI string, e.g. Linux_x86-gcc3
 *
 * See:
 * https://developer.mozilla.org/en/Bundles#Platform-specific_files
 * https://developer.mozilla.org/en/Chrome_Registration#abi
 * https://developer.mozilla.org/en/XPCOM_ABI#ABI_Naming
 *
 * See also:
 * https://bugzilla.mozilla.org/728600
 */
int main(int argc, char **argv) {
	nsresult rv;

	nsCOMPtr<nsIServiceManager> servMan;
	rv = NS_InitXPCOM2(getter_AddRefs(servMan), nsnull, nsnull);
	if (!NS_SUCCEEDED(rv)) return rv;

	nsCOMPtr<nsIXULRuntime> xulrun = do_GetService(XULAPPINFO_SERVICE_CONTRACTID, &rv);
	if (!NS_SUCCEEDED(rv)) return rv;

	nsCString xpcomAbi;
	nsCString xpcomOs;
	rv = xulrun->GetOS(xpcomOs);
	if (!NS_SUCCEEDED(rv)) return rv;
	rv = xulrun->GetXPCOMABI(xpcomAbi);
	if (!NS_SUCCEEDED(rv)) return rv;
	printf("%s_%s\n", xpcomOs.get(), xpcomAbi.get());

	rv = NS_ShutdownXPCOM(nsnull);
	return 0;
}
