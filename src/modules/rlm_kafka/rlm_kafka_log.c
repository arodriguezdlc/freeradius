/*
 *  rlm_kafka_log	Send the accounting information to an Apache kafka
 *                  topic
 *
 *  Version:    $Id$
 *
 *  Author:     Eugenio Perez <eupm90@gmail.com>
 *              Based on Nicolas Baradakis sql_log
 *
 *  Copyright (C) 2014 Eneo Tecnologia
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <freeradius-devel/ident.h>
RCSID("$Id$")

#include <freeradius-devel/radiusd.h>
#include <freeradius-devel/modules.h>
#include <freeradius-devel/rad_assert.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <assert.h>
#include <ctype.h>
#include <limits.h>

#include "rlm_client_enrichment.h"

#include <librdkafka/rdkafka.h>
#include <librbutils/rbstring.h>

#define RB_UNUSED __attribute__((unused))
#define MAX_INT_STRING_SIZE sizeof("18.446.744.073.709.551.616") // 2^64;
static const char *MAC_RADIUS_KEY = "Calling-Station-Id";

static int kafka_log_instantiate(CONF_SECTION *conf, void **instance);
static int kafka_log_detach(void *instance);
static int kafka_log_accounting(void *instance, REQUEST *request);

#ifndef LOG_DEBUG
#define LOG_DEBUG 7
#endif

#define MAX_QUERY_LEN 4096

/*
 *	Define a structure for our module configuration.
 */
typedef struct rlm_kafka_log_config_t {
	char		*brokers;
	char		*topic;
	char        *rdkafka_opts;
	int 		port;
	int 		print_delivery;

	client_enrichment_db_t *client_enrichment_db;

	rd_kafka_t       *rk;
	rd_kafka_topic_t *rkt;

	CONF_SECTION	*conf_section;
} rlm_kafka_log_config_t;

/*
 *	A mapping of configuration file names to internal variables.
 */
static const CONF_PARSER module_config[] = {
	{"brokers", PW_TYPE_STRING_PTR,
	 offsetof(rlm_kafka_log_config_t,brokers), NULL, "localhost"},
	{"topic", PW_TYPE_STRING_PTR,
	 offsetof(rlm_kafka_log_config_t,topic), NULL, NULL},
	{"port", PW_TYPE_INTEGER,
	 offsetof(rlm_kafka_log_config_t,port), NULL, 0},
	{"print_delivery", PW_TYPE_INTEGER,
	 offsetof(rlm_kafka_log_config_t,print_delivery), NULL, 0},
	{"rdkafka_opts", PW_TYPE_STRING_PTR,
	 offsetof(rlm_kafka_log_config_t,rdkafka_opts), NULL, NULL},

	{ NULL, -1, 0, NULL, NULL }	/* end the list */
};

static void msg_delivered (rd_kafka_t *rk RB_UNUSED,
	void *payload RB_UNUSED, size_t len,
	int error_code,
	void *opaque, void *msg_opaque RB_UNUSED) {

	rlm_kafka_log_config_t *inst = (rlm_kafka_log_config_t *)opaque;

	if (error_code)
		radlog(L_ERR, "%% Message delivery failed: %s\n",
		rd_kafka_err2str(error_code));
	else if (!inst->print_delivery)
		radlog(L_INFO, "%% Message delivered (%zd bytes)\n", len);
}

static int validate_mac_separator(int iter,size_t pos,
	size_t toks, char sep) {

	if(toks == 6 && iter < 5) {
		/* 00:00:00:00:00:00 or 00-00-00-00-00-00 */
		return pos % 3 != 0 && (sep == '-' || sep == ':');
	} else if (toks == 2 && iter == 0) {
		/* 000000-000000 */
		return pos == strlen("000000") && sep == '-';
	} else {
		return -1;
	}
}

static int64_t colon_hypen_mac_partitioner0(const void *const_keydata,
	size_t keylen, size_t toks) {
	/* Partition mac like 00:00:00:00:00:00 or 00-00-00-00-00-00 */
	char keydata[sizeof "00:00:00:00:00:00"];
	int64_t ret = 0;
	size_t i=0;
	char *endptr = keydata;

	assert(toks==6 || toks==2);
	memcpy(keydata,const_keydata,sizeof(keydata)-1);
	keydata[sizeof(keydata)-1] = '\0';

	for(i=0;i<toks;i++) {
		ret <<= (toks == 6 ? 8 : 24);
		const char *prev_endptr = endptr;
		ret += strtoul(endptr,&endptr,16);

		if (NULL == endptr) {
			radlog(L_ERR,"Invalid mac %.*s",(int)keylen,
				(const char *)keydata);
			return -1;
		} else if (toks == 6 && endptr - prev_endptr != 2) {
			radlog(L_ERR,
				"Invalid mac %.*s, tok %zu is not 2 char length",
				(int)keylen,(const char *)keydata,i);
			return -1;
		} else if (toks == 2 && endptr - prev_endptr != 6) {
			radlog(L_ERR,
				"Invalid mac %.*s, tok %zu is not 6 char length",
				(int)keylen,(const char *)keydata,i);
			return -1;
		} else if (!validate_mac_separator(i,endptr - keydata,toks,
								*endptr)) {
			radlog(L_ERR,
				"Invalid mac %.*s, it does not have ':' or '-' separator in pos %zu",
				(int)keylen,(const char *)keydata,i);
			return -1;
		} else if (i == toks - 1
				       && endptr - keydata != (signed)keylen) {
			radlog(L_ERR,
				"Invalid mac %.*s, last token is too short",
				(int)keylen,(const char *)keydata);
			return -1;
		}

		endptr += strlen(":");
	}

	return ret;
}

static int64_t number_mac_partitioner(const void *const_keydata,
							       size_t keylen) {
	/* Partition mac like 000000000000 */
	char keydata[sizeof "000000000000"];
	char *endptr = NULL;

	memcpy(keydata,const_keydata,sizeof(keydata) - 1);
	keydata[sizeof(keydata) - 1] = '\0';

	if(!isdigit(((const char *)keydata)[0])) {
		radlog(L_ERR,"Mac %.*s does not start with number",
				(int)keylen,(const char *)keydata);
		return -1;
	}

	const int64_t ret = strtoull(keydata,&endptr,16);

	if(endptr != keydata + keylen) {
		radlog(L_ERR,"Invalid mac %.*s, token is too short",
				(int)keylen,(const char *)keydata);
		return -1;
	}

	return ret;
}

static int64_t mac_partitioner0 (const void *keydata,size_t keylen) {
	switch(keylen) {
		/* case strlen(00-00-00-00-00-00): */
		case sizeof("00:00:00:00:00:00") - 1:
			return colon_hypen_mac_partitioner0(keydata,keylen,
								6 /*toks */);

		case sizeof("000000-000000") - 1:
			return colon_hypen_mac_partitioner0(keydata,keylen,
								2 /*toks */);

		case sizeof("000000000000") - 1:
			return number_mac_partitioner(keydata,keylen);

		default:
			radlog(L_ERR,"Invalid mac %.*s len",(int)keylen,
							(const char *)keydata);
			return -1;
	};
}

static int32_t mac_partitioner (const rd_kafka_topic_t *rkt,
		const void *keydata,size_t keylen,int32_t partition_cnt,
		void *rkt_opaque,void *msg_opaque) {
	const int partition_id = mac_partitioner0(keydata,keylen);
	if (partition_id < 0) {
		return rd_kafka_msg_partitioner_random(rkt,keydata,keylen,
			partition_cnt,rkt_opaque,msg_opaque);
	} else {
		return partition_id % partition_cnt;
	}
}

/* Extracted from Magnus Edenhill's kafkacat */
static rd_kafka_conf_res_t rdkafka_add_attr_to_config(rd_kafka_conf_t *rk_conf,
				rd_kafka_topic_conf_t *topic_conf,const char *name,const char *val,char *errstr,size_t errstr_size){
	if (!strcmp(name, "list") ||
	    !strcmp(name, "help")) {
		rd_kafka_conf_properties_show(stdout);
		exit(0);
	}

	rd_kafka_conf_res_t res = RD_KAFKA_CONF_UNKNOWN;
	/* Try "topic." prefixed properties on topic
	 * conf first, and then fall through to global if
	 * it didnt match a topic configuration property. */
	if (!strncmp(name, "topic.", strlen("topic.")))
		res = rd_kafka_topic_conf_set(topic_conf,
					      name+strlen("topic."),
					      val,errstr,errstr_size);

	if (res == RD_KAFKA_CONF_UNKNOWN)
		res = rd_kafka_conf_set(rk_conf, name, val,
					errstr, errstr_size);

    return res;
}

static int cf_section_rdkafka_parse(CONF_SECTION *section, 
						rd_kafka_conf_t *rk_conf,rd_kafka_topic_conf_t *rkt_conf,
						char *err,size_t errsize){
	static const char *config_prefix = "rdkafka.";
	CONF_PAIR *pair = cf_pair_find(section, NULL);

	while(pair){
		if(!strncmp(cf_pair_attr(pair),config_prefix,strlen(config_prefix))){
			const char *key = cf_pair_attr(pair);
			const char *val = cf_pair_value(pair);

			if(strlen(key) > strlen(config_prefix)){
				key = key + strlen(config_prefix);
				const rd_kafka_conf_res_t rc = 
					rdkafka_add_attr_to_config(rk_conf,rkt_conf,key,val,err,errsize);
				if(rc != RD_KAFKA_CONF_OK)
					return rc;
			}
		}
		pair = cf_pair_find_next(section,pair,NULL);
	}

	return RD_KAFKA_CONF_OK;
}

static int cf_client_enrichment_db_add(client_enrichment_db_t *db,const char *key,const char *val){
	return client_enrichment_db_add(db,key,val);
}

/// @TODO join with cf_section_rdkafka_parse
static int cf_section_enrichment_parse(CONF_SECTION *section, client_enrichment_db_t *db){
	static const char *config_prefix = "enrichment.";
	CONF_PAIR *pair = cf_pair_find(section, NULL);

	while(pair){
		if(!strncmp(cf_pair_attr(pair),config_prefix,strlen(config_prefix))){
			const char *key = cf_pair_attr(pair);
			const char *val = cf_pair_value(pair);

			if(strlen(key) > strlen(config_prefix)){
				key = key + strlen(config_prefix);
				cf_client_enrichment_db_add(db,key,val);
			}
		}
		pair = cf_pair_find_next(section,pair,NULL);
	}

	return RD_KAFKA_CONF_OK;
}

static int kafka_log_instantiate_kafka(CONF_SECTION *conf, rlm_kafka_log_config_t *inst){
	rd_kafka_conf_t *kafka_conf = rd_kafka_conf_new();
	rd_kafka_topic_conf_t *topic_conf = rd_kafka_topic_conf_new();
	char errstr[512];

	if(!inst){
		radlog(L_ERR, "rlm_kafka_log: inst not set in instantiate_kafka()");
		kafka_log_detach(inst);
		return -1;
	}

	if(!inst->brokers){
		radlog(L_ERR, "rlm_kafka_log: Kafka broker not set");
		kafka_log_detach(inst);
		return -1;
	}

	if(!inst->topic){
		radlog(L_ERR, "rlm_kafka_log: Kafka topic not set");
		kafka_log_detach(inst);
		return -1;
	}

	rd_kafka_conf_set_opaque (kafka_conf, inst);
	rd_kafka_conf_set_dr_cb(kafka_conf, msg_delivered);
	rd_kafka_topic_conf_set_partitioner_cb(topic_conf, mac_partitioner);

	const int rc = cf_section_rdkafka_parse(conf,kafka_conf,topic_conf,errstr,sizeof(errstr));
	if(rc != RD_KAFKA_CONF_OK){
        radlog(L_ERR,"rlm_kafka_log: Failed to add librdkafka config: %s", errstr);
        kafka_log_detach(inst);
        return -1;
	}

	inst->rk = rd_kafka_new(RD_KAFKA_PRODUCER, kafka_conf, errstr, sizeof(errstr));
	if(NULL == inst->rk) {
		radlog(L_ERR, "rlm_kafka_log: Failed to create new producer: %s\n",errstr);
		kafka_log_detach(inst);
		return -1;
	}

	if (rd_kafka_brokers_add(inst->rk, inst->brokers) == 0) {
		radlog(L_ERR, "rlm_kafka_log: No valid brokers specified\n");
		kafka_log_detach(inst);
		return -1;
	}

	inst->rkt = rd_kafka_topic_new(inst->rk, inst->topic, topic_conf);
	if(NULL == inst->rkt){
		radlog(L_ERR, "rlm_kafka_log: inst->rkt could not be created\n");
		kafka_log_detach(inst);
		return -1;
	}

	return 0;
}

/*
 *	Do any per-module initialization that is separate to each
 *	configured instance of the module.  e.g. set up connections
 *	to external databases, read configuration files, set up
 *	dictionary entries, etc.
 *
 *	If configuration information is given in the config section
 *	that must be referenced in later calls, store a handle to it
 *	in *instance otherwise put a null pointer there.
 */
static int kafka_log_instantiate(CONF_SECTION *conf, void **instance)
{
	rlm_kafka_log_config_t	*inst;

	/*
	 *      Set up a storage area for instance data.
	 */
	inst = calloc(1, sizeof(rlm_kafka_log_config_t));
	if (inst == NULL) {
	        radlog(L_ERR, "rlm_kafka_log: Not enough memory");
	        return -1;
	}

	/*
	 *	If the configuration parameters can't be parsed,
	 *	then fail.
	 */
	if (cf_section_parse(conf, inst, module_config) < 0) {
		radlog(L_ERR, "rlm_kafka_log: Unable to parse parameters");
		kafka_log_detach(inst);
		return -1;
	}

	inst->client_enrichment_db = client_enrichment_db_new();
	if (NULL == inst->client_enrichment_db) {
		radlog(L_ERR, "rlm_kafka_log: Unable to allocate client enrichment db");
		kafka_log_detach(inst);
		return -1;
	}

	cf_section_enrichment_parse(conf,inst->client_enrichment_db);

	inst->conf_section = conf;
	*instance = inst;

	const int krc = kafka_log_instantiate_kafka(conf,inst);
	if(krc<0)
		return krc;

	return 0;
}

static void wait_last_kafka_messages(rd_kafka_t *rk){
	int outq_len = rd_kafka_outq_len(rk);
	while (outq_len > 0){
		rd_kafka_poll(rk, 100);
		const int current_outq_len = rd_kafka_outq_len(rk);
		if(outq_len == current_outq_len)
			break; // Nothing to do
		outq_len = current_outq_len;
	}
}

/*
 *	Say goodbye to the cruel world.
 */
static int kafka_log_detach(void *instance)
{
	int i;
	char **p;
	rlm_kafka_log_config_t *inst = (rlm_kafka_log_config_t *)instance;

	/*
	 *	Free up dynamically allocated string pointers.
	 */
	for (i = 0; module_config[i].name != NULL; i++) {
		if (module_config[i].type != PW_TYPE_STRING_PTR) {
			continue;
		}

		/*
		 *	Treat 'config' as an opaque array of bytes,
		 *	and take the offset into it.  There's a
		 *      (char*) pointer at that offset, and we want
		 *	to point to it.
		 */
		p = (char **) (((char *)inst) + module_config[i].offset);
		if (!*p) { /* nothing allocated */
			continue;
		}
		free(*p);
		*p = NULL;
	}

	if(inst){
		if(inst->rk){
			wait_last_kafka_messages(inst->rk);
			rd_kafka_destroy(inst->rk);
			rd_kafka_wait_destroyed(2000);
		}
		free(inst);

		if(inst->client_enrichment_db){
			client_enrichment_db_free(inst->client_enrichment_db);
		}
	}
	return 0;
}

/*
 *	Write the line into kafka
 *  Based on kafkacat produce() function.
 */
static int kafka_log_produce(rlm_kafka_log_config_t *inst, strbuffer_t *buffer,
	const char *mac, size_t mac_len)
{
	const size_t  line_len = strbuffer_length(buffer);
	char   *line     = strbuffer_steal_value(buffer);
	int retried = 0;

    /* Produce message: keep trying until it succeeds. */
    do {
        rd_kafka_resp_err_t err;
        const int rc = rd_kafka_produce(inst->rkt, RD_KAFKA_PARTITION_UA,
			RD_KAFKA_MSG_F_FREE, (void *)line, line_len, mac,
			mac_len, NULL);
        if(rc != -1) {
            break;
        }

        err = rd_kafka_errno2err(errno);

        if (err != RD_KAFKA_RESP_ERR__QUEUE_FULL || retried){
			radlog(L_ERR, "rlm_kafka_log: Failed to produce message (%zd bytes): %s",
                      line_len, rd_kafka_err2str(err));
			strbuffer_close(buffer);
			return RLM_MODULE_FAIL;
		}

        retried++;

        /* Internal queue full, sleep to allow
         * messages to be produced/time out
         * before trying again. */
        rd_kafka_poll(inst->rk, 5);
    } while (!retried);

    free(buffer);

    /* Poll for delivery reports, errors, etc. */
    rd_kafka_poll(inst->rk, 0);

	return RLM_MODULE_OK;
}

#if 0
/* Escape strcpy escaping characters:
 *  \ -> \\
 */
static size_t escaped_strlcpy(char *buffer,const char *src,const ssize_t buf_len){
    char *cursor = buffer;

	while(cursor-buffer < buf_len+3 && *src != '\0'){
		switch(*cursor){
		case '\'':
			*(cursor++) = '\\';
			*(cursor++) = '\\';
			src++;
			break;
		default:
			*cursor++ = *src++;
			break;
		};
	}

	*cursor = '\0';
	return cursor-buffer;
}
#endif

// Function: C++ version 0.4 char* style "itoa", Written by Lukás Chmela. (Modified)
static char* _itoa(int64_t value, char* result, int base, size_t bufsize) {
    // check that the base if valid
    if (base < 2 || base > 36) { *result = '\0'; return result; }

    char *ptr = result+bufsize;
    int64_t tmp_value;

    *--ptr = '\0';
    do {
        tmp_value = value;
        value /= base;
        *--ptr = "zyxwvutsrqponmlkjihgfedcba9876543210123456789abcdefghijklmnopqrstuvwxyz" [35 + (tmp_value - value * base)];
    } while ( value );


    if (tmp_value < 0) *--ptr = '-';
    return ptr;
}

static void strbuffer_append_timestamp(strbuffer_t *strbuff,time_t timestamp){
	char buffer[MAX_INT_STRING_SIZE]; /* 2^64 */
	const char *timestamp_str = _itoa(timestamp,buffer,10,sizeof(buffer));
	
	strbuffer_append(strbuff,"\"timestamp\":");
	strbuffer_append(strbuff,timestamp_str);
}

static void strbuffer_append_packet_type(strbuffer_t *strbuff,const RADIUS_PACKET *packet){
	strbuffer_append(strbuff,"\"packet_type\":");
	if ((packet->code > 0) && (packet->code < FR_MAX_PACKET_CODE)){
		strbuffer_append(strbuff,"\"");
		strbuffer_append(strbuff,fr_packet_codes[packet->code]);
		strbuffer_append(strbuff,"\"");
	}else{
		char buffer[MAX_INT_STRING_SIZE];
		const char *packet_code = _itoa(packet->code,buffer,10,sizeof(buffer));
		strbuffer_append(strbuff,packet_code);
	}
}

/* return: 1 if data added. 0 IOC */
static int strbuffer_append_value_pair(strbuffer_t *strbuff, VALUE_PAIR *pair,
	const char **value_appended, size_t *value_appended_len) {
	char buffer[1024];
	const size_t buffer_len = vp_prints_value(buffer, sizeof(buffer), pair, 0 /* quote */);

	if(buffer_len>0){
		strbuffer_append(strbuff,",");
		strbuffer_append(strbuff,"\"");
		strbuffer_append(strbuff,pair->name);
		strbuffer_append(strbuff,"\":\"");

		if (value_appended) {
			/* Current pos */
			*value_appended = strbuffer_value(strbuff) +
				strbuffer_length(strbuff);
		}

		if (value_appended_len) {
			*value_appended_len = buffer_len;
		}

		strbuffer_append_escaped_bytes(strbuff,buffer,buffer_len,"\\\"");
		strbuffer_append(strbuff,"\"");
		return 1;
	}
	return 0;
}

static strbuffer_t *packet2buffer(const REQUEST *request,
		const char *enrichment, size_t enrichment_len,
		const char **mac, size_t *mac_len) {
	const RADIUS_PACKET *packet = request->packet;
	VALUE_PAIR	*pair;

	strbuffer_t *buffer = malloc(sizeof(*buffer));
	strbuffer_init(buffer);

	strbuffer_append(buffer,"{");
	strbuffer_append_timestamp(buffer,time(NULL));
	strbuffer_append(buffer,",");
	strbuffer_append_packet_type(buffer,packet);

	VALUE_PAIR src_vp, dst_vp;

	memset(&src_vp, 0, sizeof(src_vp));
	memset(&dst_vp, 0, sizeof(dst_vp));
	src_vp.operator = dst_vp.operator = T_OP_EQ;

	switch (packet->src_ipaddr.af) {
	case AF_INET:
		src_vp.name = "Packet-Src-IP-Address";
		src_vp.type = PW_TYPE_IPADDR;
		src_vp.attribute = PW_PACKET_SRC_IP_ADDRESS;
		src_vp.vp_ipaddr = packet->src_ipaddr.ipaddr.ip4addr.s_addr;
		dst_vp.name = "Packet-Dst-IP-Address";
		dst_vp.type = PW_TYPE_IPADDR;
		dst_vp.attribute = PW_PACKET_DST_IP_ADDRESS;
		dst_vp.vp_ipaddr = packet->dst_ipaddr.ipaddr.ip4addr.s_addr;
		break;
	case AF_INET6:
		src_vp.name = "Packet-Src-IPv6-Address";
		src_vp.type = PW_TYPE_IPV6ADDR;
		src_vp.attribute = PW_PACKET_SRC_IPV6_ADDRESS;
		memcpy(src_vp.vp_strvalue,
		       &packet->src_ipaddr.ipaddr.ip6addr,
		       sizeof(packet->src_ipaddr.ipaddr.ip6addr));
		dst_vp.name = "Packet-Dst-IPv6-Address";
		dst_vp.type = PW_TYPE_IPV6ADDR;
		dst_vp.attribute = PW_PACKET_DST_IPV6_ADDRESS;
		memcpy(dst_vp.vp_strvalue,
		       &packet->dst_ipaddr.ipaddr.ip6addr,
		       sizeof(packet->dst_ipaddr.ipaddr.ip6addr));
		break;
	default:
		break;
	}

	strbuffer_append_value_pair(buffer,&src_vp, NULL, NULL);
	strbuffer_append_value_pair(buffer,&dst_vp, NULL, NULL);

	src_vp.name = "Packet-Src-IP-Port";
	src_vp.attribute = PW_PACKET_SRC_PORT;
	src_vp.type = PW_TYPE_INTEGER;
	src_vp.vp_integer = packet->src_port;
	dst_vp.name = "Packet-Dst-IP-Port";
	dst_vp.attribute = PW_PACKET_DST_PORT;
	dst_vp.type = PW_TYPE_INTEGER;
	dst_vp.vp_integer = packet->dst_port;

	strbuffer_append_value_pair(buffer,&src_vp, NULL, NULL);
	strbuffer_append_value_pair(buffer,&dst_vp, NULL, NULL);

	/* Write each attribute/value to the buffer */
	for (pair = packet->vps; pair != NULL; pair = pair->next) {
		if(0==strcmp(pair->name,MAC_RADIUS_KEY)) {
			/* Setting mac in msg as key */
			strbuffer_append_value_pair(buffer, pair, mac,
				mac_len);
		} else {
			strbuffer_append_value_pair(buffer, pair, NULL, NULL);
		}
	}

	/* Write enrichment data */
	if(enrichment) {
		strbuffer_append_bytes(buffer,enrichment,enrichment_len);
	}


	strbuffer_append(buffer,"}");
	return buffer;
}

/*
 *	Write accounting information to this module's database.
 */
static int kafka_log_accounting(void *instance, REQUEST *request)
{
	rlm_kafka_log_config_t	*inst = (rlm_kafka_log_config_t *)instance;
	VALUE_PAIR	*pair;
	DICT_VALUE	*dval;

	rad_assert(request != NULL);
	rad_assert(request->packet != NULL);

	RDEBUG("Processing kafka_log_accounting");

	/* Find the Acct Status Type. */
	if ((pair = pairfind(request->packet->vps, PW_ACCT_STATUS_TYPE)) == NULL) {
		radlog_request(L_ERR, 0, request, "Packet has no account status type");
		return RLM_MODULE_INVALID;
	}

	/* Search the query in conf section of the module */
	if ((dval = dict_valbyattr(PW_ACCT_STATUS_TYPE, pair->vp_integer)) == NULL) {
		radlog_request(L_ERR, 0, request, "Unsupported Acct-Status-Type = %d",
			       pair->vp_integer);
		return RLM_MODULE_NOOP;
	}

	const char *mac = NULL;
	size_t mac_len = 0;
	const char *enrichment = client_enrichment_db_get(inst->client_enrichment_db,
		request->client->shortname);
	size_t enrichment_len = enrichment?strlen(enrichment):0;
	strbuffer_t *json_buffer = packet2buffer(request,enrichment,
		enrichment_len, &mac, &mac_len);
	if(json_buffer){
		return kafka_log_produce(inst, json_buffer, mac, mac_len);
	}else{
		return RLM_MODULE_NOOP;
	}
}

static int kafka_log_post_proxy(void *instance, REQUEST *request){
	rlm_kafka_log_config_t	*inst = (rlm_kafka_log_config_t *)instance;

	rad_assert(request != NULL);
	rad_assert(request->packet != NULL);

	RDEBUG("Processing kafka_log_post_proxy");

	/* Find the Acct Status Type. */
	/*
	if ((pair = pairfind(request->packet->vps, PW_POST_PROXY_TYPE)) == NULL) {
		radlog_request(L_ERR, 0, request, "Packet has no post-proxy type");
		return RLM_MODULE_INVALID;
	}
	*/

	/* Search the query in conf section of the module */
	/*
	if ((dval = dict_valbyattr(PW_POST_PROXY_TYPE, pair->vp_integer)) == NULL) {
		radlog_request(L_ERR, 0, request, "Unsupported Post-Proxy-Type = %d",
			       pair->vp_integer);
		return RLM_MODULE_NOOP;
	}
	*/

	const char *mac = NULL;
	size_t mac_len = 0;
	strbuffer_t *json_buffer = packet2buffer(request,NULL, 0, &mac,
								&mac_len);
	if(json_buffer) {
		kafka_log_produce(inst, json_buffer,mac, mac_len);
		return RLM_MODULE_OK;
	} else {
		return RLM_MODULE_NOOP;
	}
}

/*
 *	The module name should be the only globally exported symbol.
 *	That is, everything else should be 'static'.
 *
 *	If the module needs to temporarily modify it's instantiation
 *	data, the type should be changed to RLM_TYPE_THREAD_UNSAFE.
 *	The server will then take care of ensuring that the module
 *	is single-threaded.
 */
module_t rlm_kafka_log = {
	RLM_MODULE_INIT,
	"kafka_log",
	RLM_TYPE_CHECK_CONFIG_SAFE | RLM_TYPE_HUP_SAFE,		/* type */
	kafka_log_instantiate,		/* instantiation */
	kafka_log_detach,			/* detach */
	{
		NULL,			/* authentication */
		NULL,			/* authorization */
		NULL,			/* preaccounting */
		kafka_log_accounting,	/* accounting */
		NULL,			/* checksimul */
		NULL,			/* pre-proxy */
		kafka_log_post_proxy,	/* post-proxy */
		NULL			/* post-auth */
	},
};
