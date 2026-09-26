#include <pspsdk.h>
#include <pspkernel.h>
#include <pspiofilemgr_kernel.h>
#include <pspwlan.h>
#include <psputility.h>
#include <string.h>
#include "config.h"
#include "logs.h"
#include "systemctrl.h"

#define SERVER_CONFIG_PATH "ms0:/seplugins/atpro_last_ip.txt"
#define LEGACY_SERVER_CONFIG_PATH "ms0:/seplugins/server.txt"
#define DEFAULT_SERVER_HOST "coldbird.uk.to"
#define SERVER_HOST_LENGTH 128
#define OSK_TEXT_LENGTH 128

typedef int (*UtilityDialogStatus)(void);
typedef int (*UtilityDialogUpdate)(int);
typedef int (*UtilityDialogShutdown)(void);
typedef int (*UtilityNetconfInitStart)(pspUtilityNetconfData *);
typedef int (*UtilityOskInitStart)(SceUtilityOskParams *);

static UtilityDialogStatus netconf_get_status;
static UtilityDialogUpdate netconf_update;
static UtilityDialogShutdown netconf_shutdown;
static UtilityNetconfInitStart netconf_init;
static UtilityDialogStatus osk_get_status;
static UtilityDialogUpdate osk_update;
static UtilityDialogShutdown osk_shutdown;
static UtilityOskInitStart osk_init;
static volatile int dialog_active;
static volatile int cancel_requested;
static char server_host[SERVER_HOST_LENGTH];

static int resolve_utility_functions(void)
{
	netconf_init = (UtilityNetconfInitStart)sctrlHENFindFunction("sceUtility_Driver", "sceUtility", 0x4DB1E739);
	netconf_get_status = (UtilityDialogStatus)sctrlHENFindFunction("sceUtility_Driver", "sceUtility", 0x6332AA39);
	netconf_update = (UtilityDialogUpdate)sctrlHENFindFunction("sceUtility_Driver", "sceUtility", 0x91E70E35);
	netconf_shutdown = (UtilityDialogShutdown)sctrlHENFindFunction("sceUtility_Driver", "sceUtility", 0xF88155F6);
	
	osk_init = (UtilityOskInitStart)sctrlHENFindFunction("sceUtility_Driver", "sceUtility", 0xF6269B82);
	osk_get_status = (UtilityDialogStatus)sctrlHENFindFunction("sceUtility_Driver", "sceUtility", 0xF3F76017);
	osk_update = (UtilityDialogUpdate)sctrlHENFindFunction("sceUtility_Driver", "sceUtility", 0x4B85C861);
	osk_shutdown = (UtilityDialogShutdown)sctrlHENFindFunction("sceUtility_Driver", "sceUtility", 0x3DFAEBA9);
	
	return netconf_init != NULL && netconf_get_status != NULL && netconf_update != NULL && netconf_shutdown != NULL &&
		osk_init != NULL && osk_get_status != NULL && osk_update != NULL && osk_shutdown != NULL ? 0 : -1;
}

static int valid_server_host(const char * host)
{
	int length = 0;
	int index = 0;

	if(host == NULL) return 0;
	length = strlen(host);
	if(length == 0 || length >= SERVER_HOST_LENGTH) return 0;

	for(index = 0; index < length; index++)
	{
		char value = host[index];
		if(!((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
			(value >= '0' && value <= '9') || value == '.' || value == '-')) return 0;
	}

	return 1;
}

static void load_server_host(void)
{
	static const char * paths[] = { SERVER_CONFIG_PATH, LEGACY_SERVER_CONFIG_PATH };
	int path_index = 0;

	for(; path_index < sizeof(paths) / sizeof(paths[0]); path_index++)
	{
		SceUID file = sceIoOpen(paths[path_index], PSP_O_RDONLY, 0);
		if(file >= 0)
		{
			int length = sceIoRead(file, server_host, sizeof(server_host) - 1);
			sceIoClose(file);
			if(length > 0)
			{
				int index = 0;
				server_host[length] = 0;
				for(index = 0; index < length; index++)
				{
					if(server_host[index] == '\r' || server_host[index] == '\n')
					{
						server_host[index] = 0;
						break;
					}
				}
				if(valid_server_host(server_host)) return;
			}
		}
	}

	strcpy(server_host, DEFAULT_SERVER_HOST);
}

static int save_server_host(const char * host)
{
	SceUID file = sceIoOpen(SERVER_CONFIG_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
	int length = strlen(host);
	int offset = 0;
	if(file < 0) return file;

	while(offset < length)
	{
		int written = sceIoWrite(file, host + offset, length - offset);
		if(written <= 0)
		{
			sceIoClose(file);
			return -1;
		}
		offset += written;
	}

	sceIoWrite(file, "\n", 1);
	sceIoClose(file);
	return 0;
}

static void copy_ascii_to_osk(unsigned short * target, int capacity, const char * source)
{
	int index = 0;
	for(; index < capacity - 1 && source[index] != 0; index++) target[index] = (unsigned char)source[index];
	target[index] = 0;
}

static int copy_osk_to_ascii(char * target, int capacity, const unsigned short * source)
{
	int index = 0;
	for(; index < capacity - 1 && source[index] != 0; index++)
	{
		if(source[index] > 0x7F) return -1;
		target[index] = source[index];
	}
	if(index == capacity - 1 && source[index] != 0) return -1;
	target[index] = 0;
	return index;
}

static int wait_for_dialog(UtilityDialogStatus get_status, UtilityDialogUpdate update, UtilityDialogShutdown shutdown)
{
	int shutdown_started = 0;
	for(;;)
	{
		int status = get_status();
		if(status < 0) return status;
		if(status == PSP_UTILITY_DIALOG_NONE) return 0;
		if(status == PSP_UTILITY_DIALOG_VISIBLE && !cancel_requested) update(1);
		if((status == PSP_UTILITY_DIALOG_QUIT || cancel_requested) && !shutdown_started)
		{
			shutdown();
			shutdown_started = 1;
		}
		sceKernelDelayThread(10000);
	}
}

static int show_network_selection(void)
{
	pspUtilityNetconfData params;
	struct pspUtilityNetconfAdhoc adhoc_params;
	int result = 0;
	memset(&params, 0, sizeof(params));
	memset(&adhoc_params, 0, sizeof(adhoc_params));
	params.base.size = sizeof(params);
	params.base.language = PSP_SYSTEMPARAM_LANGUAGE_ENGLISH;
	params.base.buttonSwap = PSP_UTILITY_ACCEPT_CROSS;
	params.base.graphicsThread = 17;
	params.base.accessThread = 19;
	params.base.fontThread = 18;
	params.base.soundThread = 16;
	params.action = PSP_NETCONF_ACTION_CONNECTAP;
	params.adhocparam = &adhoc_params;

	result = netconf_init(&params);
	if(result < 0) return result;
	return wait_for_dialog(netconf_get_status, netconf_update, netconf_shutdown);
}

static int show_server_host_osk(void)
{
	SceUtilityOskParams params;
	SceUtilityOskData data;
	unsigned short description[OSK_TEXT_LENGTH];
	unsigned short initial_text[OSK_TEXT_LENGTH];
	unsigned short output_text[OSK_TEXT_LENGTH];
	char confirmed_host[SERVER_HOST_LENGTH];
	int result = 0;
	memset(&params, 0, sizeof(params));
	memset(&data, 0, sizeof(data));
	memset(description, 0, sizeof(description));
	memset(initial_text, 0, sizeof(initial_text));
	memset(output_text, 0, sizeof(output_text));
	copy_ascii_to_osk(description, OSK_TEXT_LENGTH, "Server IP or host");
	copy_ascii_to_osk(initial_text, OSK_TEXT_LENGTH, server_host);

	data.language = PSP_UTILITY_OSK_LANGUAGE_DEFAULT;
	data.inputtype = PSP_UTILITY_OSK_INPUTTYPE_LATIN_DIGIT | PSP_UTILITY_OSK_INPUTTYPE_LATIN_SYMBOL |
		PSP_UTILITY_OSK_INPUTTYPE_LATIN_LOWERCASE | PSP_UTILITY_OSK_INPUTTYPE_LATIN_UPPERCASE;
	data.lines = 1;
	data.unk_24 = 1;
	data.desc = description;
	data.intext = initial_text;
	data.outtextlength = OSK_TEXT_LENGTH;
	data.outtext = output_text;
	data.outtextlimit = SERVER_HOST_LENGTH - 1;

	params.base.size = sizeof(params);
	params.base.language = PSP_SYSTEMPARAM_LANGUAGE_ENGLISH;
	params.base.buttonSwap = PSP_UTILITY_ACCEPT_CROSS;
	params.base.graphicsThread = 17;
	params.base.accessThread = 19;
	params.base.fontThread = 18;
	params.base.soundThread = 16;
	params.datacount = 1;
	params.data = &data;

	result = osk_init(&params);
	if(result < 0) return result;
	result = wait_for_dialog(osk_get_status, osk_update, osk_shutdown);
	if(result < 0 || cancel_requested) return result;
	if(data.result != PSP_UTILITY_OSK_RESULT_CHANGED && data.result != PSP_UTILITY_OSK_RESULT_UNCHANGED) return 0;
	if(copy_osk_to_ascii(confirmed_host, sizeof(confirmed_host), data.result == PSP_UTILITY_OSK_RESULT_CHANGED ? output_text : initial_text) < 0 || !valid_server_host(confirmed_host)) return -1;
	strcpy(server_host, confirmed_host);
	return save_server_host(server_host);
}

int atpro_configure_network(void)
{
	uint32_t k1 = 0;
	int result = -1;
	if(sceWlanGetSwitchState() != 1) return result;
	k1 = pspSdkSetK1(0);
	cancel_requested = 0;
	dialog_active = 1;
	load_server_host();
	if(resolve_utility_functions() == 0)
	{
		result = show_network_selection();
		if(result >= 0 && !cancel_requested) result = show_server_host_osk();
	}
	else printk("ATPRO: PSP utility dialogs unavailable\n");
	dialog_active = 0;
	pspSdkSetK1(k1);
	return result;
}

int atpro_config_dialog_active(void)
{
	return dialog_active;
}

void atpro_config_cancel(void)
{
	cancel_requested = 1;
}