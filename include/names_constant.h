/*
 * names_constant.h
 */
#include <sys/types.h>

extern enum_names ip_protocol_id_names;	/* aka ipproto_*; in ip_protocol.c */

extern enum_names sa_policy_bit_names;
extern enum_names dpd_action_names;
#ifdef KERNEL_XFRM
extern enum_names xfrm_policy_names;
#endif
extern enum_names sd_action_names;
extern enum_names stf_status_names;
extern enum_names keyword_auth_names;
extern enum_names keyword_host_names;

extern const enum_names debug_names;
extern const enum_names debug_help;
extern const struct lmod_info debug_lmod_info;

extern enum_names shunt_policy_names;		/* SHUNT_... */
extern enum_names shunt_policy_percent_names;	/* %... */
extern enum_names connection_kind_names;
extern enum_names routing_story;
extern enum_names certpolicy_type_names;
extern enum_names oakley_attr_names;
extern enum_names oakley_attr_bit_names;
extern enum_names *const oakley_attr_val_descs[];
extern const unsigned int oakley_attr_val_descs_roof;
extern enum_names ipsec_attr_names;
extern enum_names *const ipsec_attr_val_descs[];
extern enum_names sa_lifetime_names;
extern enum_names encapsulation_mode_names;
extern enum_names auth_alg_names;
extern enum_names oakley_lifetime_names;

extern enum_names ike_version_names;

extern enum_names version_names;
extern enum_names doi_names;
extern enum_names ikev1_payload_names;
extern enum_names ikev2_payload_names;
extern enum_enum_names payload_type_names;
extern enum_names ikev2_last_proposal_desc;
extern enum_names ikev2_last_transform_desc;
extern enum_names payload_names_ikev1orv2;
extern enum_names attr_msg_type_names;
extern enum_names modecfg_attr_names;
extern enum_names xauth_type_names;
extern enum_names xauth_attr_names;
extern enum_names ikev1_exchange_names;
extern enum_names ikev2_exchange_names;
extern enum_names isakmp_xchg_type_names;
extern enum_enum_names exchange_type_names;
extern enum_names ikev1_protocol_names;
extern enum_names isakmp_transformid_names;
extern enum_names ah_transformid_names;
extern enum_names esp_transformid_names;
extern enum_names ipsec_ipcomp_algo_names;
extern enum_names ike_cert_type_names;
extern enum_names oakley_enc_names;
extern enum_names oakley_hash_names;
extern enum_names oakley_auth_names;
extern enum_names oakley_group_names;
extern enum_names v1_notification_names;

/* IKEv2 */
extern enum_names ikev2_auth_method_names;
extern enum_names ikev2_hash_algorithm_names;
extern enum_names ikev2_proposal_protocol_id_names;	/* 1=IKE SA, 2=AH, 3=ESP */
extern enum_names ikev2_delete_protocol_id_names;	/* 1=IKE SA, 2=AH, 3=ESP */
extern enum_names ikev2_notify_protocol_id_names;	/* NONE=0, 1=IKE, 2=AH, 3=ESP */
extern enum_names ikev2_trans_type_names;
extern enum_names ikev2_trans_type_encr_names;
extern enum_names ikev2_trans_type_prf_names;
extern enum_names ikev2_trans_type_integ_names;
extern enum_names ikev2_trans_type_esn_names;
extern enum_names ikev2_trans_attr_descs;
extern enum_enum_names v2_transform_ID_enums;
extern enum_names ikev2_cert_type_names;
extern enum_names v2_notification_names;
extern enum_names ikev2_ts_type_names;
extern enum_names ikev2_cp_type_names;
extern enum_names ikev2_cp_attribute_type_names;
extern enum_names ikev2_redirect_gw_names;
extern enum_names allow_global_redirect_names;

extern enum_names dns_auth_level_names;

extern enum_names eap_code_names;
extern enum_names eap_type_names;

/*
 * Attribute Type "constant" for Security Context
 *
 * Originally, we assigned the value 10, but that properly belongs to ECN_TUNNEL.
 * We then assigned 32001 which is in the private range RFC 2407.
 * Unfortunately, we feel we have to support 10 as an option for backward
 * compatibility.
 * This variable specifies (globally!!) which we support: 10 or 32001.
 * ??? surely that makes migration to 32001 all or nothing.
 */
extern uint16_t secctx_attr_type;

extern enum_names natt_method_names;

extern enum_names secret_kind_names;
extern enum_names ikev2_ppk_id_type_names;

/* natt traversal types */
extern const char *const natt_type_bitnames[];
