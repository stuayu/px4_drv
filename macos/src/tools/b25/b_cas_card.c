#include "b_cas_card.h"
#include "b_cas_card_error_code.h"

#include <stdlib.h>
#include <string.h>

#include <math.h>

#if defined(WIN32)
	#include <windows.h>
#endif
#include <PCSC/winscard.h>


/*+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
 inner structures
 ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++*/
typedef struct {
	
	SCARDCONTEXT       mng;
	SCARDHANDLE        card;
	uint32_t           protocol;

	uint8_t           *pool;
	char              *reader;

	uint8_t           *sbuf;
	uint8_t           *rbuf;

	B_CAS_INIT_STATUS  stat;
	
	B_CAS_ID           id;
	int32_t            id_max;

	B_CAS_PWR_ON_CTRL_INFO pwc;
	int32_t            pwc_max;
	int32_t            no_smart_card;
	
} B_CAS_CARD_PRIVATE_DATA;

/*+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
 constant values
 ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++*/
static const uint8_t INITIAL_SETTING_CONDITIONS_CMD[] = {
	0x90, 0x30, 0x00, 0x00, 0x00,
};

static const uint8_t CARD_ID_INFORMATION_ACQUIRE_CMD[] = {
	0x90, 0x32, 0x00, 0x00, 0x00,
};

static const uint8_t POWER_ON_CONTROL_INFORMATION_REQUEST_CMD[] = {
	0x90, 0x80, 0x00, 0x00, 0x01, 0x00, 0x00,
};

static const uint8_t ECM_RECEIVE_CMD_HEADER[] = {
	0x90, 0x34, 0x00, 0x00,
};

static const uint8_t EMM_RECEIVE_CMD_HEADER[] = {
	0x90, 0x36, 0x00, 0x00,
};

#define B_CAS_BUFFER_MAX (4*1024)

/*+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
 function prottypes (interface method)
 ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++*/
static void release_b_cas_card(void *bcas);
static int init_b_cas_card(void *bcas);
static int get_init_status_b_cas_card(void *bcas, B_CAS_INIT_STATUS *stat);
static int get_id_b_cas_card(void *bcas, B_CAS_ID *dst);
static int get_pwr_on_ctrl_b_cas_card(void *bcas, B_CAS_PWR_ON_CTRL_INFO *dst);
static int proc_ecm_b_cas_card(void *bcas, B_CAS_ECM_RESULT *dst, uint8_t *src, int len);
static int proc_emm_b_cas_card(void *bcas, uint8_t *src, int len);

/*+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
 global function implementation
 ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++*/
B_CAS_CARD *create_b_cas_card()
{
	int n;
	
	B_CAS_CARD *r;
	B_CAS_CARD_PRIVATE_DATA *prv;

	n = sizeof(B_CAS_CARD) + sizeof(B_CAS_CARD_PRIVATE_DATA);
	prv = (B_CAS_CARD_PRIVATE_DATA *)calloc(1, n);
	if(prv == NULL){
		return NULL;
	}

	r = (B_CAS_CARD *)(prv+1);

	r->private_data = prv;

	r->release = release_b_cas_card;
	r->init = init_b_cas_card;
	r->get_init_status = get_init_status_b_cas_card;
	r->get_id = get_id_b_cas_card;
	r->get_pwr_on_ctrl = get_pwr_on_ctrl_b_cas_card;
	r->proc_ecm = proc_ecm_b_cas_card;
	r->proc_emm = proc_emm_b_cas_card;

	return r;
}

/*+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
 function prottypes (private method)
 ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++*/
static B_CAS_CARD_PRIVATE_DATA *private_data(void *bcas);
static void teardown(B_CAS_CARD_PRIVATE_DATA *prv);
static int change_id_max(B_CAS_CARD_PRIVATE_DATA *prv, int max);
static int change_pwc_max(B_CAS_CARD_PRIVATE_DATA *prv, int max);
static int connect_card(B_CAS_CARD_PRIVATE_DATA *prv, const char *reader_name);
static void extract_power_on_ctrl_response(B_CAS_PWR_ON_CTRL *dst, uint8_t *src);
static void extract_mjd(int *yy, int *mm, int *dd, int mjd);
static int setup_ecm_receive_command(uint8_t *dst, uint8_t *src, int len);
static int setup_emm_receive_command(uint8_t *dst, uint8_t *src, int len);
static int32_t load_be_uint16(uint8_t *p);
static int64_t load_be_uint48(uint8_t *p);
static const SCARD_IO_REQUEST *active_pci(B_CAS_CARD_PRIVATE_DATA *prv);

/*+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
 interface method implementation
 ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++*/
static void release_b_cas_card(void *bcas)
{
	B_CAS_CARD_PRIVATE_DATA *prv;

	prv = private_data(bcas);
	if(prv == NULL){
		/* do nothing */
		return;
	}

	teardown(prv);
	free(prv);
}

static int init_b_cas_card(void *bcas)
{
	int m;
	long ret;
	uint32_t len;
	
	B_CAS_CARD_PRIVATE_DATA *prv;

	prv = private_data(bcas);
	if(prv == NULL){
		return B_CAS_CARD_ERROR_INVALID_PARAMETER;
	}

	teardown(prv);

	ret = SCardEstablishContext(SCARD_SCOPE_USER, NULL, NULL, &(prv->mng));
	if(ret != SCARD_S_SUCCESS){
		return B_CAS_CARD_ERROR_NO_SMART_CARD_READER;
	}

	ret = SCardListReaders(prv->mng, NULL, NULL, &len);
	if(ret != SCARD_S_SUCCESS){
		return B_CAS_CARD_ERROR_NO_SMART_CARD_READER;
	}
	len += 256;
	
	m = len + (2*B_CAS_BUFFER_MAX) + (sizeof(int64_t)*16) + (sizeof(B_CAS_PWR_ON_CTRL)*16);
	prv->pool = (uint8_t *)malloc(m);
	if(prv->pool == NULL){
		return B_CAS_CARD_ERROR_NO_ENOUGH_MEMORY;
	}

	prv->reader = (char *)(prv->pool);
	prv->sbuf = prv->pool + len;
	prv->rbuf = prv->sbuf + B_CAS_BUFFER_MAX;
	prv->id.data = (int64_t *)(prv->rbuf + B_CAS_BUFFER_MAX);
	prv->id_max = 16;
	prv->pwc.data = (B_CAS_PWR_ON_CTRL *)(prv->id.data + prv->id_max);
	prv->pwc_max = 16;

	ret = SCardListReaders(prv->mng, NULL, prv->reader, &len);
	if(ret != SCARD_S_SUCCESS){
		return B_CAS_CARD_ERROR_NO_SMART_CARD_READER;
	}
	prv->no_smart_card = 0;

	while( prv->reader[0] != 0 ){
		SCARD_READERSTATE rs;
		memset(&rs, 0, sizeof(rs));
		rs.szReader = prv->reader;
		rs.dwCurrentState = SCARD_STATE_UNAWARE;
		ret = SCardGetStatusChange(prv->mng, 0, &rs, 1);
		if(ret == SCARD_S_SUCCESS){
			if((rs.dwEventState & SCARD_STATE_PRESENT) == 0){
				prv->no_smart_card = 1;
				prv->reader += (strlen(prv->reader) + 1);
				continue;
			}
		}
		if(connect_card(prv, prv->reader)){
			break;
		}
		prv->reader += (strlen(prv->reader) + 1);
	}

	if(prv->card == 0){
		if(prv->no_smart_card){
			return B_CAS_CARD_ERROR_NO_SMART_CARD;
		}
		return B_CAS_CARD_ERROR_ALL_READERS_CONNECTION_FAILED;
	}

	return 0;
}

static int get_init_status_b_cas_card(void *bcas, B_CAS_INIT_STATUS *stat)
{
	B_CAS_CARD_PRIVATE_DATA *prv;

	prv = private_data(bcas);
	if( (prv == NULL) || (stat == NULL) ){
		return B_CAS_CARD_ERROR_INVALID_PARAMETER;
	}

	if(prv->card == 0){
		return B_CAS_CARD_ERROR_NOT_INITIALIZED;
	}

	memcpy(stat, &(prv->stat), sizeof(B_CAS_INIT_STATUS));

	return 0;
}

static int get_id_b_cas_card(void *bcas, B_CAS_ID *dst)
{
	long ret;
	
	uint32_t slen;
	uint32_t rlen;

	int i,num;

	uint8_t *p;
	uint8_t *tail;
	
	B_CAS_CARD_PRIVATE_DATA *prv;
	const SCARD_IO_REQUEST *pci;
	SCARD_IO_REQUEST sir;

	prv = private_data(bcas);
	if( (prv == NULL) || (dst == NULL) ){
		return B_CAS_CARD_ERROR_INVALID_PARAMETER;
	}

	if(prv->card == 0){
		return B_CAS_CARD_ERROR_NOT_INITIALIZED;
	}
	pci = active_pci(prv);
	if(pci == NULL){
		return B_CAS_CARD_ERROR_TRANSMIT_FAILED;
	}

	slen = sizeof(CARD_ID_INFORMATION_ACQUIRE_CMD);
	memcpy(prv->sbuf, CARD_ID_INFORMATION_ACQUIRE_CMD, slen);
	memcpy(&sir, pci, sizeof(sir));
	rlen = B_CAS_BUFFER_MAX;

	ret = SCardTransmit(prv->card, pci, prv->sbuf, slen, &sir, prv->rbuf, &rlen);
	if( (ret != SCARD_S_SUCCESS) || (rlen < 19) ){
		return B_CAS_CARD_ERROR_TRANSMIT_FAILED;
	}

	p = prv->rbuf + 6;
	tail = prv->rbuf + rlen;
	if( p+1 > tail ){
		return B_CAS_CARD_ERROR_TRANSMIT_FAILED;
	}

	num = p[0];
	if(num > prv->id_max){
		if(change_id_max(prv, num+4) < 0){
			return B_CAS_CARD_ERROR_NO_ENOUGH_MEMORY;
		}
	}
	
	p += 1;
	for(i=0;i<num;i++){
		if( p+10 > tail ){
			return B_CAS_CARD_ERROR_TRANSMIT_FAILED;
		}
		
		{
			int maker_id;
			int version;
			int check_code;
			
			maker_id = p[0];
			version = p[1];
			prv->id.data[i] = load_be_uint48(p+2);
			check_code = load_be_uint16(p+8);
		}
		
		p += 10;
	}

	prv->id.count = num;

	memcpy(dst, &(prv->id), sizeof(B_CAS_ID));

	return 0;
}

static int get_pwr_on_ctrl_b_cas_card(void *bcas, B_CAS_PWR_ON_CTRL_INFO *dst)
{
	long ret;
	
	uint32_t slen;
	uint32_t rlen;

	int i,num,code;

	B_CAS_CARD_PRIVATE_DATA *prv;
	const SCARD_IO_REQUEST *pci;
	SCARD_IO_REQUEST sir;

	memset(dst, 0, sizeof(B_CAS_PWR_ON_CTRL_INFO));

	prv = private_data(bcas);
	if( (prv == NULL) || (dst == NULL) ){
		return B_CAS_CARD_ERROR_INVALID_PARAMETER;
	}

	if(prv->card == 0){
		return B_CAS_CARD_ERROR_NOT_INITIALIZED;
	}
	pci = active_pci(prv);
	if(pci == NULL){
		return B_CAS_CARD_ERROR_TRANSMIT_FAILED;
	}

	slen = sizeof(POWER_ON_CONTROL_INFORMATION_REQUEST_CMD);
	memcpy(prv->sbuf, POWER_ON_CONTROL_INFORMATION_REQUEST_CMD, slen);
	prv->sbuf[5] = 0;
	memcpy(&sir, pci, sizeof(sir));
	rlen = B_CAS_BUFFER_MAX;

	ret = SCardTransmit(prv->card, pci, prv->sbuf, slen, &sir, prv->rbuf, &rlen);
	if( (ret != SCARD_S_SUCCESS) || (rlen < 18) || (prv->rbuf[6] != 0) ){
		return B_CAS_CARD_ERROR_TRANSMIT_FAILED;
	}

	code = load_be_uint16(prv->rbuf+4);
	if(code == 0xa101){
		/* no data */
		return 0;
	}else if(code != 0x2100){
		return B_CAS_CARD_ERROR_TRANSMIT_FAILED;
	}

	num = (prv->rbuf[7] + 1);
	if(prv->pwc_max < num){
		if(change_pwc_max(prv, num+4) < 0){
			return B_CAS_CARD_ERROR_NO_ENOUGH_MEMORY;
		}
	}

	extract_power_on_ctrl_response(prv->pwc.data+0, prv->rbuf);

	for(i=1;i<num;i++){
		prv->sbuf[5] = i;
		rlen = B_CAS_BUFFER_MAX;

		ret = SCardTransmit(prv->card, pci, prv->sbuf, slen, &sir, prv->rbuf, &rlen);
		if( (ret != SCARD_S_SUCCESS) || (rlen < 18) || (prv->rbuf[6] != i) ){
			return B_CAS_CARD_ERROR_TRANSMIT_FAILED;
		}

		extract_power_on_ctrl_response(prv->pwc.data+i, prv->rbuf);
	}

	prv->pwc.count = num;

	memcpy(dst, &(prv->pwc), sizeof(B_CAS_PWR_ON_CTRL_INFO));

	return 0;
}

static int proc_ecm_b_cas_card(void *bcas, B_CAS_ECM_RESULT *dst, uint8_t *src, int len)
{
	int retry_count;
	
	long ret;
	uint32_t slen;
	uint32_t rlen;
	
	B_CAS_CARD_PRIVATE_DATA *prv;
	const SCARD_IO_REQUEST *pci;

	SCARD_IO_REQUEST sir;

	prv = private_data(bcas);
	if( (prv == NULL) ||
	    (dst == NULL) ||
	    (src == NULL) ||
	    (len < 1) ){
		return B_CAS_CARD_ERROR_INVALID_PARAMETER;
	}

	if(prv->card == 0){
		return B_CAS_CARD_ERROR_NOT_INITIALIZED;
	}
	pci = active_pci(prv);
	if(pci == NULL){
		return B_CAS_CARD_ERROR_TRANSMIT_FAILED;
	}

	slen = setup_ecm_receive_command(prv->sbuf, src, len);
	memcpy(&sir, pci, sizeof(sir));
	rlen = B_CAS_BUFFER_MAX;

	retry_count = 0;
	ret = SCardTransmit(prv->card, pci, prv->sbuf, slen, &sir, prv->rbuf, &rlen);
	while( ((ret != SCARD_S_SUCCESS) || (rlen < 25)) && (retry_count < 10) ){
		retry_count += 1;
		if(!connect_card(prv, prv->reader)){
			continue;
		}
		pci = active_pci(prv);
		if(pci == NULL){
			continue;
		}
		slen = setup_ecm_receive_command(prv->sbuf, src, len);
		memcpy(&sir, pci, sizeof(sir));
		rlen = B_CAS_BUFFER_MAX;

		ret = SCardTransmit(prv->card, pci, prv->sbuf, slen, &sir, prv->rbuf, &rlen);
	}

	if( (ret != SCARD_S_SUCCESS) || (rlen < 25) ){
		return B_CAS_CARD_ERROR_TRANSMIT_FAILED;
	}

	memcpy(dst->scramble_key, prv->rbuf+6, 16);
	dst->return_code = load_be_uint16(prv->rbuf+4);

	return 0;
}

static int proc_emm_b_cas_card(void *bcas, uint8_t *src, int len)
{
	int retry_count;
	
	long ret;
	uint32_t slen;
	uint32_t rlen;
	
	B_CAS_CARD_PRIVATE_DATA *prv;
	const SCARD_IO_REQUEST *pci;

	SCARD_IO_REQUEST sir;

	prv = private_data(bcas);
	if( (prv == NULL) ||
	    (src == NULL) ||
	    (len < 1) ){
		return B_CAS_CARD_ERROR_INVALID_PARAMETER;
	}

	if(prv->card == 0){
		return B_CAS_CARD_ERROR_NOT_INITIALIZED;
	}
	pci = active_pci(prv);
	if(pci == NULL){
		return B_CAS_CARD_ERROR_TRANSMIT_FAILED;
	}

	slen = setup_emm_receive_command(prv->sbuf, src, len);
	memcpy(&sir, pci, sizeof(sir));
	rlen = B_CAS_BUFFER_MAX;

	retry_count = 0;
	ret = SCardTransmit(prv->card, pci, prv->sbuf, slen, &sir, prv->rbuf, &rlen);
	while( ((ret != SCARD_S_SUCCESS) || (rlen < 6)) && (retry_count < 2) ){
		retry_count += 1;
		if(!connect_card(prv, prv->reader)){
			continue;
		}
		pci = active_pci(prv);
		if(pci == NULL){
			continue;
		}
		slen = setup_emm_receive_command(prv->sbuf, src, len);
		memcpy(&sir, pci, sizeof(sir));
		rlen = B_CAS_BUFFER_MAX;

		ret = SCardTransmit(prv->card, pci, prv->sbuf, slen, &sir, prv->rbuf, &rlen);
	}

	if( (ret != SCARD_S_SUCCESS) || (rlen < 6) ){
		return B_CAS_CARD_ERROR_TRANSMIT_FAILED;
	}

	return 0;
}

/*+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
 private method implementation
 ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++*/
static B_CAS_CARD_PRIVATE_DATA *private_data(void *bcas)
{
	B_CAS_CARD_PRIVATE_DATA *r;
	B_CAS_CARD *p;

	p = (B_CAS_CARD *)bcas;
	if(p == NULL){
		return NULL;
	}

	r = (B_CAS_CARD_PRIVATE_DATA *)(p->private_data);
	if( ((void *)(r+1)) != ((void *)p) ){
		return NULL;
	}

	return r;
}

static void teardown(B_CAS_CARD_PRIVATE_DATA *prv)
{
	if(prv->card != 0){
		SCardDisconnect(prv->card, SCARD_LEAVE_CARD);
		prv->card = 0;
		prv->protocol = 0;
	}

	if(prv->mng != 0){
		SCardReleaseContext(prv->mng);
		prv->mng = 0;
		prv->protocol = 0;
	}

	if(prv->pool != NULL){
		free(prv->pool);
		prv->pool = NULL;
	}

	prv->reader = NULL;
	prv->sbuf = NULL;
	prv->rbuf = NULL;
	prv->id.data = NULL;
	prv->id_max = 0;
	prv->no_smart_card = 0;
}

static int change_id_max(B_CAS_CARD_PRIVATE_DATA *prv, int max)
{
	int m;
	int reader_size;
	int pwctrl_size;
	
	uint8_t *p;
	uint8_t *old_reader;
	uint8_t *old_pwctrl;

	reader_size = prv->sbuf - prv->pool;
	pwctrl_size = prv->pwc.count * sizeof(B_CAS_PWR_ON_CTRL);

	m  = reader_size;
	m += (2*B_CAS_BUFFER_MAX);
	m += (max*sizeof(int64_t));
	m += (prv->pwc_max*sizeof(B_CAS_PWR_ON_CTRL));
	p = (uint8_t *)malloc(m);
	if(p == NULL){
		return B_CAS_CARD_ERROR_NO_ENOUGH_MEMORY;
	}

	old_reader = (uint8_t *)(prv->reader);
	old_pwctrl = (uint8_t *)(prv->pwc.data);

	prv->reader = (char *)p;
	prv->sbuf = p + reader_size;
	prv->rbuf = prv->sbuf + B_CAS_BUFFER_MAX;
	prv->id.data = (int64_t *)(prv->rbuf + B_CAS_BUFFER_MAX);
	prv->id_max = max;
	prv->pwc.data = (B_CAS_PWR_ON_CTRL *)(prv->id.data + prv->id_max);

	memcpy(prv->reader, old_reader, reader_size);
	memcpy(prv->pwc.data, old_pwctrl, pwctrl_size);
	
	free(prv->pool);
	prv->pool = p;

	return 0;
}

static int change_pwc_max(B_CAS_CARD_PRIVATE_DATA *prv, int max)
{
	int m;
	int reader_size;
	int cardid_size;
	
	uint8_t *p;
	uint8_t *old_reader;
	uint8_t *old_cardid;

	reader_size = prv->sbuf - prv->pool;
	cardid_size = prv->id.count * sizeof(int64_t);

	m  = reader_size;
	m += (2*B_CAS_BUFFER_MAX);
	m += (prv->id_max*sizeof(int64_t));
	m += (max*sizeof(B_CAS_PWR_ON_CTRL));
	p = (uint8_t *)malloc(m);
	if(p == NULL){
		return B_CAS_CARD_ERROR_NO_ENOUGH_MEMORY;
	}

	old_reader = (uint8_t *)(prv->reader);
	old_cardid = (uint8_t *)(prv->id.data);

	prv->reader = (char *)p;
	prv->sbuf = p + reader_size;
	prv->rbuf = prv->sbuf + B_CAS_BUFFER_MAX;
	prv->id.data = (int64_t *)(prv->rbuf + B_CAS_BUFFER_MAX);
	prv->pwc.data = (B_CAS_PWR_ON_CTRL *)(prv->id.data + prv->id_max);
	prv->pwc_max = max;

	memcpy(prv->reader, old_reader, reader_size);
	memcpy(prv->id.data, old_cardid, cardid_size);
	
	free(prv->pool);
	prv->pool = p;

	return 0;
}

static int connect_card(B_CAS_CARD_PRIVATE_DATA *prv, const char *reader_name)
{
	int m,n;
	
	long ret;
	uint32_t rlen,protocol;

	uint8_t *p;
	
	SCARD_IO_REQUEST sir;

	if(prv->card != 0){
		SCardDisconnect(prv->card, SCARD_RESET_CARD);
		prv->card = 0;
	}

	ret = SCardConnect(prv->mng, reader_name, SCARD_SHARE_SHARED, SCARD_PROTOCOL_T0|SCARD_PROTOCOL_T1, &(prv->card), &protocol);
	if(ret != SCARD_S_SUCCESS){
		if(ret == SCARD_E_NO_SMARTCARD){
			prv->no_smart_card = 1;
		}
		return 0;
	}
	prv->protocol = protocol;
	prv->no_smart_card = 0;

	m = sizeof(INITIAL_SETTING_CONDITIONS_CMD);
	memcpy(prv->sbuf, INITIAL_SETTING_CONDITIONS_CMD, m);
	{
		const SCARD_IO_REQUEST *pci;
		pci = active_pci(prv);
		if(pci == NULL){
			SCardDisconnect(prv->card, SCARD_RESET_CARD);
			prv->card = 0;
			prv->protocol = 0;
			return 0;
		}
		memcpy(&sir, pci, sizeof(sir));
	rlen = B_CAS_BUFFER_MAX;
		ret = SCardTransmit(prv->card, pci, prv->sbuf, m, &sir, prv->rbuf, &rlen);
	}
	if(ret != SCARD_S_SUCCESS){
		return 0;
	}

	if(rlen < 57){
		return 0;
	}
	
	p = prv->rbuf;

	n = load_be_uint16(p+4);
	if(n != 0x2100){ // return code missmatch
		return 0;
	}

	memcpy(prv->stat.system_key, p+16, 32);
	memcpy(prv->stat.init_cbc, p+48, 8);
	prv->stat.bcas_card_id = load_be_uint48(p+8);
	prv->stat.card_status = load_be_uint16(p+2);

	return 1;
}

static const SCARD_IO_REQUEST *active_pci(B_CAS_CARD_PRIVATE_DATA *prv)
{
	if(prv->protocol == SCARD_PROTOCOL_T1){
		return SCARD_PCI_T1;
	}
	if(prv->protocol == SCARD_PROTOCOL_T0){
		return SCARD_PCI_T0;
	}
	return NULL;
}

static void extract_power_on_ctrl_response(B_CAS_PWR_ON_CTRL *dst, uint8_t *src)
{
	int referrence;
	int start;
	int limit;
	
	
	dst->broadcaster_group_id = src[8];
	referrence = (src[9]<<8)|src[10];
	start = referrence - src[11];
	limit = start + (src[12]-1);

	extract_mjd(&(dst->s_yy), &(dst->s_mm), &(dst->s_dd), start);
	extract_mjd(&(dst->l_yy), &(dst->l_mm), &(dst->l_dd), limit);

	dst->hold_time = src[13];
	dst->network_id = (src[14]<<8)|src[15];
	dst->transport_id = (src[16]<<8)|src[17];
	
}

static void extract_mjd(int *yy, int *mm, int *dd, int mjd)
{
	int yd;
	int md;
	int d;

	yd = (int)floor( (mjd-15078.2) / 365.25 );
	md = (int)floor( ((mjd-14956.1) - (yd*365.25)) / 30.6001 );
	d = mjd - 14956 - (int)floor(yd*365.25) - (int)floor(md*30.6001);
	if( md > 13 ){
		*dd = d;
		*mm = md-13;
		*yy = 1900+yd+1;
	}else{
		*dd = d;
		*mm = md-1;
		*yy = 1900+yd;
	}

	if(*yy < 2000){ /* mjd bit overflow - retry */
		extract_mjd(yy, mm, dd, mjd+0x10000);
	}
}

static int setup_ecm_receive_command(uint8_t *dst, uint8_t *src, int len)
{
	int r;
	
	r  = sizeof(ECM_RECEIVE_CMD_HEADER);
	memcpy(dst+0, ECM_RECEIVE_CMD_HEADER, r);
	dst[r] = (uint8_t)(len & 0xff);
	r += 1;
	memcpy(dst+r, src, len);
	r += len;
	dst[r] = 0;
	r += 1;

	return r;
}

static int setup_emm_receive_command(uint8_t *dst, uint8_t *src, int len)
{
	int r;

	r  = sizeof(EMM_RECEIVE_CMD_HEADER);
	memcpy(dst+0, EMM_RECEIVE_CMD_HEADER, r);
	dst[r] = (uint8_t)(len & 0xff);
	r += 1;
	memcpy(dst+r, src, len);
	r += len;
	dst[r] = 0;
	r += 1;

	return r;
}

static int32_t load_be_uint16(uint8_t *p)
{
	return ((p[0]<<8)|p[1]);
}

static int64_t load_be_uint48(uint8_t *p)
{
	int i;
	int64_t r;

	r = p[0];
	for(i=1;i<6;i++){
		r <<= 8;
		r |= p[i];
	}

	return r;
}

