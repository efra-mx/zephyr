/* © 2020 Endian Technologies AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 */

#include <logging/log.h>
LOG_MODULE_DECLARE(modem_gsm, CONFIG_MODEM_LOG_LEVEL);


#include <zephyr.h>
#include <stdlib.h>
#include <stdio.h>
#include <drivers/gpio.h>

#include "modem_cmd_handler.h"
#include "modem_context.h"
#include "drivers/modem/ublox_sara_r4.h"

#define MDM_URAT_LENGTH          16
#define MDM_UBANDMASKS           2

#define GSM_CMD_SETUP_TIMEOUT K_SECONDS(2)

struct modem_info {
	int mdm_mnoprof;
	int mdm_psm;
	char mdm_urat[MDM_URAT_LENGTH];
	uint64_t mdm_bandmask[MDM_UBANDMASKS];
	int mdm_signal;
	int mdm_simcard_status;
	int mdm_roaming;
	int mdm_service;
};

static struct modem_info minfo;

K_SEM_DEFINE(ublox_sem, 0, 1);

/* Handler: +UMNOPROF: <mnoprof> */
MODEM_CMD_DEFINE(on_cmd_atcmdinfo_mnoprof)
{
	size_t out_len;
	char buf[16];
	char *prof;

	out_len = net_buf_linearize(buf,
				    sizeof(buf) - 1,
				    data->rx_buf, 0, len);
	buf[out_len] = '\0';
	prof = strchr(buf, ':');
	if (!prof || *(prof + 1) != ' ') {
		minfo.mdm_mnoprof = -1;
		return -EINVAL;
	}
	prof = prof + 2;
	minfo.mdm_mnoprof = atoi(prof);
	LOG_INF("MNO profile: %d", minfo.mdm_mnoprof);

	k_sem_give(&ublox_sem);
	return 0;
}

/* Handler: +CPSMS: <mode>,[...] */
MODEM_CMD_DEFINE(on_cmd_atcmdinfo_psm)
{
	size_t out_len;
	char buf[16];
	char *psm;

	out_len = net_buf_linearize(buf,
				    sizeof(buf) - 1,
				    data->rx_buf, 0, len);
	buf[out_len] = '\0';

	psm = strchr(buf, ':');
	if (!psm) {
		return -EINVAL;
	}
	minfo.mdm_psm = *(psm + 1) - '0';
	LOG_INF("PSM mode: %d", minfo.mdm_psm);

	k_sem_give(&ublox_sem);
	return 0;
}

static int gsm_setup_mnoprof(struct modem_context *ctx, struct k_sem *sem)
{
	int ret;
	struct setup_cmd cmds[] = {
		SETUP_CMD("AT+UMNOPROF?", "", on_cmd_atcmdinfo_mnoprof, 0U, ""),
	};

	ret = modem_cmd_handler_setup_cmds_nolock(&ctx->iface,
						  &ctx->cmd_handler,
						  cmds,
						  ARRAY_SIZE(cmds),
						  &ublox_sem,
						  GSM_CMD_SETUP_TIMEOUT);
	if (ret < 0) {
		LOG_ERR("AT+UMNOPROF ret:%d", ret);
		return ret;
	}

	if (minfo.mdm_mnoprof != -1 && minfo.mdm_mnoprof != CONFIG_MODEM_GSM_MNOPROF) {
		/* The wrong MNO profile was set, change it */
		LOG_WRN("Changing MNO profile from %d to %d",
			minfo.mdm_mnoprof, CONFIG_MODEM_GSM_MNOPROF);

		/* Detach from the network */
		ret = modem_cmd_send_nolock(&ctx->iface,
					    &ctx->cmd_handler,
					    NULL, 0,
					    "AT+CFUN=0",
					    sem,
					    K_SECONDS(2));
		if (ret < 0) {
			LOG_ERR("AT+CFUN=0 ret:%d", ret);
		}

		/* Set the profile */
		ret = modem_cmd_send_nolock(&ctx->iface,
					    &ctx->cmd_handler,
					    NULL, 0,
					    "AT+UMNOPROF=" STRINGIFY(CONFIG_MODEM_GSM_MNOPROF),
					    sem,
					    K_SECONDS(2));
		if (ret < 0) {
			LOG_ERR("AT+UMNOPROF ret:%d", ret);
		}

		/* Reboot */
		ret = modem_cmd_send_nolock(&ctx->iface,
					    &ctx->cmd_handler,
					    NULL, 0,
					    "AT+CFUN=15",
					    sem,
					    K_SECONDS(2));
		if (ret < 0) {
			LOG_ERR("AT+CFUN=15 ret:%d", ret);
		}
		k_sleep(K_SECONDS(3));

		return -EAGAIN;
	}

	return ret;
}

static int gsm_setup_psm(struct modem_context *ctx, struct k_sem *sem)
{
	int ret;
	struct setup_cmd query_cmds[] = {
		SETUP_CMD("AT+CPSMS?", "", on_cmd_atcmdinfo_psm, 0U, ""),
	};
	struct setup_cmd set_cmds[] = {
		SETUP_CMD_NOHANDLE("ATE0"),
		SETUP_CMD_NOHANDLE("AT+CFUN=0"),
		SETUP_CMD_NOHANDLE("AT+CPSMS=0"),
		SETUP_CMD_NOHANDLE("AT+CFUN=15"),
	};

	ret = modem_cmd_handler_setup_cmds_nolock(&ctx->iface,
						  &ctx->cmd_handler,
						  query_cmds,
						  ARRAY_SIZE(query_cmds),
						  &ublox_sem,
						  GSM_CMD_SETUP_TIMEOUT);
	if (ret < 0) {
		LOG_ERR("Querying PSM ret:%d", ret);
		return ret;
	}

	if (minfo.mdm_psm == 1) {
		LOG_WRN("Disabling PSM");
		ret = modem_cmd_handler_setup_cmds_nolock(&ctx->iface,
							  &ctx->cmd_handler,
							  set_cmds,
							  ARRAY_SIZE(set_cmds),
							  sem,
							  GSM_CMD_SETUP_TIMEOUT);
		if (ret < 0) {
			LOG_ERR("Querying PSM ret:%d", ret);
			return ret;
		}

		k_sleep(K_SECONDS(3));

		return -EAGAIN;
	}

	return ret;
}

/* Handler: +URAT: <rat1>,[...] */
MODEM_CMD_DEFINE(on_cmd_atcmdinfo_urat)
{
	size_t out_len;

	out_len = net_buf_linearize(minfo.mdm_urat,
				    sizeof(minfo.mdm_urat) - 1,
				    data->rx_buf, 0, len);
	minfo.mdm_urat[out_len] = '\0';

	/* Get rid of "+URAT: " */
	char *p = strchr(minfo.mdm_urat, ' ');
	if (p) {
		size_t len = strlen(p + 1);
		memmove(minfo.mdm_urat, p + 1, len + 1);
	}

	LOG_INF("URAT: %s", log_strdup(minfo.mdm_urat));

	k_sem_give(&ublox_sem);
	return 0;
}

static int gsm_setup_urat(struct modem_context *ctx, struct k_sem *sem)
{
	int ret;
	struct setup_cmd query_cmds[] = {
		SETUP_CMD("AT+URAT?", "", on_cmd_atcmdinfo_urat, 0U, ""),
	};
	struct setup_cmd set_cmds[] = {
		SETUP_CMD_NOHANDLE("ATE0"),
		SETUP_CMD_NOHANDLE("AT+CFUN=0"),
		SETUP_CMD_NOHANDLE("AT+URAT=" CONFIG_MODEM_GSM_URAT),
		SETUP_CMD_NOHANDLE("AT+CFUN=15"),
	};

	ret = modem_cmd_handler_setup_cmds_nolock(&ctx->iface,
						  &ctx->cmd_handler,
						  query_cmds,
						  ARRAY_SIZE(query_cmds),
						  &ublox_sem,
						  GSM_CMD_SETUP_TIMEOUT);
	if (ret < 0) {
		LOG_ERR("Querying URAT ret:%d", ret);
		return ret;
	}

	if (strcmp(minfo.mdm_urat, CONFIG_MODEM_GSM_URAT)) {
		LOG_WRN("Setting URAT");
		ret = modem_cmd_handler_setup_cmds_nolock(&ctx->iface,
							  &ctx->cmd_handler,
							  set_cmds,
							  ARRAY_SIZE(set_cmds),
							  sem,
							  GSM_CMD_SETUP_TIMEOUT);
		if (ret < 0) {
			LOG_ERR("Setting URAT ret:%d", ret);
			return ret;
		}

		k_sleep(K_SECONDS(3));

		return -EAGAIN;
	}

	return ret;
}

/* Handler: +UBANDMASK: <rat0>,<mask>,[...] */
MODEM_CMD_DEFINE(on_cmd_atcmdinfo_ubandmask)
{
	char buf[40];
	size_t out_len;

	out_len = net_buf_linearize(buf, sizeof(buf) - 1,
				    data->rx_buf, 0, len);
	buf[out_len] = '\0';
	char *p = buf;

	/* Skip over "+UBANDMASK: " */
	if (strchr(buf, ' ')) {
		p = strchr(buf, ' ');
	}
	int i = 0;
	int rat = -1;
	while (p) {
		int v = atoi(p);

		if (i % 2 == 0) {
			rat = v;
		} else if (rat >= 0 && rat < MDM_UBANDMASKS) {
			minfo.mdm_bandmask[rat] = v;
			LOG_INF("UBANDMASK for RAT %d: 0x%x", rat, v);
		}

		p = strchr(p, ',');
		if (p) p++;
		i++;
	}

	k_sem_give(&ublox_sem);
	return 0;
}

static int gsm_setup_ubandmask(struct modem_context *ctx, struct k_sem *sem)
{
	int ret;
	struct setup_cmd query_cmds[] = {
		SETUP_CMD("AT+UBANDMASK?", "", on_cmd_atcmdinfo_ubandmask, 0U, ""),
	};
	struct setup_cmd set_cmds[] = {
		SETUP_CMD_NOHANDLE("ATE0"),
		SETUP_CMD_NOHANDLE("AT+CFUN=0"),
		SETUP_CMD_NOHANDLE("AT+UBANDMASK=0,"
				   STRINGIFY(CONFIG_MODEM_GSM_UBANDMASK_M1)),
		SETUP_CMD_NOHANDLE("AT+UBANDMASK=1,"
				   STRINGIFY(CONFIG_MODEM_GSM_UBANDMASK_NB1)),
		SETUP_CMD_NOHANDLE("AT+CFUN=15"),
	};

	ret = modem_cmd_handler_setup_cmds_nolock(&ctx->iface,
						  &ctx->cmd_handler,
						  query_cmds,
						  ARRAY_SIZE(query_cmds),
						  &ublox_sem,
						  GSM_CMD_SETUP_TIMEOUT);
	if (ret < 0) {
		LOG_ERR("Querying UBANDMASK ret:%d", ret);
		return ret;
	}

	if (minfo.mdm_bandmask[0] != CONFIG_MODEM_GSM_UBANDMASK_M1 ||
	    minfo.mdm_bandmask[1] != CONFIG_MODEM_GSM_UBANDMASK_NB1) {
		LOG_WRN("Setting UBANDMASK");
		ret = modem_cmd_handler_setup_cmds_nolock(&ctx->iface,
							  &ctx->cmd_handler,
							  set_cmds,
							  ARRAY_SIZE(set_cmds),
							  sem,
							  GSM_CMD_SETUP_TIMEOUT);
		k_sleep(K_SECONDS(3));
		if (ret < 0) {
			LOG_ERR("Setting URAT ret:%d", ret);
			return ret;
		}


		return -EAGAIN;
	}

	return ret;
}

/* Handler: +CIND: <battchg>,<signal>,<service>,<sounder>,
 *          <message>,<call>,<roam>,<smsfull>,<gprs>,
 *          <callsetup>,<callheld>,<simind>
 */
MODEM_CMD_DEFINE(on_cmd_atcmdinfo_cind)
{
	char buf[40];
	size_t out_len;

	out_len = net_buf_linearize(buf, sizeof(buf) - 1,
				    data->rx_buf, 0, len);
	buf[out_len] = '\0';

	char *p = buf;
	int i = 0;
	while (p) {
		int v = atoi(p);

		switch (i) {
		case 1:
			minfo.mdm_signal = v;
			LOG_INF("Signal strength: %d", minfo.mdm_signal);
			break;
		case 2:
			LOG_INF("Network service: %d", v);
			minfo.mdm_service = v;
			break;
		case 6:
			minfo.mdm_roaming = -1;
			if (v == 1) {
				minfo.mdm_roaming = 1;
			} else if(v == 0) {
				minfo.mdm_roaming = 0;
			}
			LOG_INF("Roaming: %d", v);
			break;
		case 11:
			LOG_INF("Simcard status: %d", v);
			minfo.mdm_simcard_status = v;
			break;
		}

		p = strchr(p, ',');
		if (p) p++;
		i++;
	}

	k_sem_give(&ublox_sem);
	return 0;
}

/* Poll the network status. Should return non-negative to indicate
 * that the network is ready to use.
 */
static int gsm_poll_network_status(struct modem_context *ctx, struct k_sem *sem)
{
	/* FIXME: During development, when you tend to be particularily rough
	 * with it, modem becomes unresponsive in this particular phase a lot.
	 * It's not recoverable apart from power cycling modem. This needs a
	 * solution.
	 */

	int ret;
	struct modem_cmd cind_cmd =
		MODEM_CMD("+CIND:", on_cmd_atcmdinfo_cind, 0U, "");

	ret = modem_cmd_send_nolock(&ctx->iface,
				    &ctx->cmd_handler,
				    &cind_cmd, 1,
				    "AT+CIND?",
				    &ublox_sem,
				    K_SECONDS(5));
	if (ret < 0) {
		LOG_ERR("Querying CIND: %d", ret);
		return ret;
	}

	if (minfo.mdm_service != 1) {
		return -EIO;
	}

	return 0;
}

int gsm_ppp_pre_connect_hook(struct modem_context *ctx, struct k_sem *sem)
{
	k_sleep(K_SECONDS(1));
	return gsm_poll_network_status(ctx, sem);
}

int gsm_ppp_setup_hook(struct modem_context *ctx, struct k_sem *sem)
{
	int ret;
	static char manual_cops[1 + sizeof("AT+COPS=1,2,\"12345\"")];
	int operator;

	ret = gsm_setup_mnoprof(ctx, sem);
	if (ret < 0) {
		LOG_WRN("gsm_setup_mnoprof returned %d", ret);
		return ret;
	}

	ret = gsm_setup_psm(ctx, sem);
	if (ret < 0) {
		LOG_WRN("gsm_setup_psm returned %d", ret);
		return ret;
	}

	ret = gsm_setup_urat(ctx, sem);
	if (ret < 0) {
		LOG_WRN("gsm_setup_urat returned %d", ret);
		return ret;
	}

	ret = gsm_setup_ubandmask(ctx, sem);
	if (ret < 0) {
		LOG_WRN("gsm_setup_ubandmask returned %d", ret);
		return ret;
	}

#if defined(CONFIG_MODEM_CACHE_OPERATOR)
	if (!ctx->data_cached_operator) {
		LOG_INF("No cached operator");
		return ret;
	}
	operator = ctx->data_cached_operator;
#else
	operator = ctx->data_operator;
#endif
	sprintf(manual_cops, "AT+COPS=1,2,%d", operator);
	LOG_INF("Manual operator cmd: %s", log_strdup(manual_cops));

	/* Best effort basis ie don't signal failure if this fails */
	(void)modem_cmd_send_nolock(&ctx->iface,
				    &ctx->cmd_handler,
				    NULL, 0,
				    manual_cops,
				    sem,
				    K_SECONDS(2));
	return ret;
}
