/**
 * Copyright 2020 The Magma Authors.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef __cplusplus
extern "C" {
#endif
#include "lte/gateway/c/core/oai/common/log.h"
#include "lte/gateway/c/core/oai/lib/itti/intertask_interface_types.h"
#include "lte/gateway/c/core/oai/lib/itti/intertask_interface.h"
#include "lte/gateway/c/core/oai/common/dynamic_memory_check.h"
#include "lte/gateway/c/core/oai/common/conversions.h"

#ifdef __cplusplus
}
#endif
#include "lte/gateway/c/core/oai/common/conversions.h"
#include "lte/gateway/c/core/oai/common/assertions.h"
#include "lte/gateway/c/core/oai/common/common_defs.h"
#include <sstream>
#include "lte/gateway/c/core/oai/tasks/amf/amf_asDefs.h"
#include "lte/gateway/c/core/oai/tasks/amf/amf_app_ue_context_and_proc.h"
#include "lte/gateway/c/core/oai/tasks/amf/amf_authentication.h"
#include "lte/gateway/c/core/oai/tasks/amf/amf_sap.h"
#include "lte/gateway/c/core/oai/tasks/amf/amf_app_timer_management.h"
#include "lte/gateway/c/core/oai/tasks/amf/amf_recv.h"
#include "lte/gateway/c/core/oai/tasks/amf/amf_identity.h"

extern amf_config_t amf_config;
namespace magma5g {
extern task_zmq_ctx_s amf_app_task_zmq_ctx;
AmfMsg amf_msg_obj;
extern std::unordered_map<imsi64_t, guti_and_amf_id_t> amf_supi_guti_map;
static int identification_t3570_handler(zloop_t* loop, int timer_id, void* arg);
int nas_proc_establish_ind(
    const amf_ue_ngap_id_t ue_id, const bool is_mm_ctx_new,
    const tai_t originating_tai, const ecgi_t ecgi,
    const m5g_rrc_establishment_cause_t as_cause, const s_tmsi_m5_t s_tmsi,
    bstring msg) {
  amf_sap_t amf_sap = {};
  uint32_t rc       = RETURNerror;
  if (msg) {
    /*
     * Notify the AMF procedure call manager that NAS signaling
     * connection establishment indication message has been received
     * from the Access-Stratum sublayer
     */
    amf_sap.primitive                           = AMFAS_ESTABLISH_REQ;
    amf_sap.u.amf_as.primitive                  = _AMFAS_ESTABLISH_REQ;
    amf_sap.u.amf_as.u.establish.ue_id          = ue_id;
    amf_sap.u.amf_as.u.establish.is_initial     = true;
    amf_sap.u.amf_as.u.establish.is_amf_ctx_new = is_mm_ctx_new;
    amf_sap.u.amf_as.u.establish.nas_msg        = msg;
    amf_sap.u.amf_as.u.establish.ecgi           = ecgi;
    amf_sap.u.amf_as.u.establish.tai            = originating_tai;
    rc                                          = amf_sap_send(&amf_sap);
  }
  OAILOG_FUNC_RETURN(LOG_NAS_AMF, rc);
}

/***************************************************************************
**                                                                        **
** Name:    nas_new_amf_procedures()                                     **
**                                                                        **
** Description: Generic function for new amf Procedures                   **
**                                                                        **
**                                                                        **
***************************************************************************/
amf_procedures_t* nas_new_amf_procedures(amf_context_t* const amf_context) {
  amf_procedures_t* amf_procedures = new amf_procedures_t();
  LIST_INIT(&amf_procedures->amf_common_procs);
  return amf_procedures;
}

//-----------------------------------------------------------------------------
static void nas5g_delete_auth_info_procedure(
    struct amf_context_s* amf_context,
    nas5g_auth_info_proc_t** auth_info_proc) {
  if (*auth_info_proc) {
    if ((*auth_info_proc)->cn_proc.base_proc.parent) {
      (*auth_info_proc)->cn_proc.base_proc.parent->child = NULL;
    }
    free_wrapper((void**) auth_info_proc);
  }
}

/***************************************************************************
**                                                                        **
** Name:    nas5g_delete_cn_procedure()                                   **
**                                                                        **
** Description: Generic function for new auth info  Procedure             **
**                                                                        **
**                                                                        **
***************************************************************************/
void nas5g_delete_cn_procedure(
    struct amf_context_s* amf_context, nas5g_cn_proc_t* cn_proc) {
  if (amf_context->amf_procedures) {
    nas5g_cn_procedure_t* p1 =
        LIST_FIRST(&amf_context->amf_procedures->cn_procs);
    nas5g_cn_procedure_t* p2 = NULL;

    while (p1) {
      p2 = LIST_NEXT(p1, entries);
      if (p1->proc == cn_proc) {
        switch (cn_proc->type) {
          case CN5G_PROC_AUTH_INFO:
            nas5g_delete_auth_info_procedure(
                amf_context, (nas5g_auth_info_proc_t**) &cn_proc);
            break;
          case CN5G_PROC_NONE:
            free_wrapper((void**) &cn_proc);
            break;
          default:;
        }
        LIST_REMOVE(p1, entries);
        free_wrapper((void**) &p1);
        return;
      }
      p1 = p2;
    }
  }
}

/***************************************************************************
**                                                                        **
** Name:    nas5g_new_cn_auth_info_procedure()                            **
**                                                                        **
** Description: Generic function for new auth info  Procedure             **
**                                                                        **
**                                                                        **
***************************************************************************/
nas5g_auth_info_proc_t* nas5g_new_cn_auth_info_procedure(
    amf_context_t* const amf_context) {
  if (!(amf_context->amf_procedures)) {
    amf_context->amf_procedures = nas_new_amf_procedures(amf_context);
  }
  nas5g_auth_info_proc_t* auth_info_proc =
      (nas5g_auth_info_proc_t*) calloc(1, sizeof(nas5g_auth_info_proc_t));

  auth_info_proc->cn_proc.base_proc.type = NAS_PROC_TYPE_CN;
  auth_info_proc->cn_proc.type           = CN5G_PROC_AUTH_INFO;

  nas5g_cn_procedure_t* wrapper =
      (nas5g_cn_procedure_t*) calloc(1, sizeof(*wrapper));
  if (wrapper) {
    wrapper->proc = &auth_info_proc->cn_proc;
    LIST_INSERT_HEAD(&amf_context->amf_procedures->cn_procs, wrapper, entries);
    return auth_info_proc;
  } else {
    free_wrapper((void**) &auth_info_proc);
  }
  return NULL;
}

/***************************************************************************
**                                                                        **
** Name:    get_nas_specific_procedure_registration()                     **
**                                                                        **
** Description: Function for NAS Specific Procedure Registration          **
**                                                                        **
**                                                                        **
***************************************************************************/
nas_amf_registration_proc_t* get_nas_specific_procedure_registration(
    const amf_context_t* ctxt) {
  if ((ctxt) && (ctxt->amf_procedures) &&
      (ctxt->amf_procedures->amf_specific_proc) &&
      ((AMF_SPEC_PROC_TYPE_REGISTRATION ==
        ctxt->amf_procedures->amf_specific_proc->type))) {
    return (nas_amf_registration_proc_t*)
        ctxt->amf_procedures->amf_specific_proc;
  }
  return NULL;
}

/***************************************************************************
**                                                                        **
** Name:    is_nas_specific_procedure_registration_running()              **
**                                                                        **
** Description: Function to check if NAS procedure registration running   **
**                                                                        **
**                                                                        **
***************************************************************************/
bool is_nas_specific_procedure_registration_running(const amf_context_t* ctxt) {
  if ((ctxt) && (ctxt->amf_procedures) &&
      (ctxt->amf_procedures->amf_specific_proc) &&
      ((AMF_SPEC_PROC_TYPE_REGISTRATION ==
        ctxt->amf_procedures->amf_specific_proc->type)))
    return true;
  return false;
}

/***************************************************************************
**                                                                        **
** Name:    nas5g_new_identification_procedure()                          **
**                                                                        **
** Description: Invokes Function for new identification procedure         **
**                                                                        **
**                                                                        **
***************************************************************************/
nas_amf_ident_proc_t* nas5g_new_identification_procedure(
    amf_context_t* const amf_context) {
  if (!(amf_context->amf_procedures)) {
    amf_context->amf_procedures = nas_new_amf_procedures(amf_context);
  }
  nas_amf_ident_proc_t* ident_proc       = new nas_amf_ident_proc_t;
  ident_proc->amf_com_proc.amf_proc.type = NAS_AMF_PROC_TYPE_COMMON;
  ident_proc->T3570.msec              = 1000 * amf_config.nas_config.t3570_sec;
  ident_proc->T3570.id                = AMF_APP_TIMER_INACTIVE_ID;
  ident_proc->amf_com_proc.type       = AMF_COMM_PROC_IDENT;
  nas_amf_common_procedure_t* wrapper = new nas_amf_common_procedure_t;
  if (wrapper) {
    wrapper->proc = &ident_proc->amf_com_proc;
    LIST_INSERT_HEAD(
        &amf_context->amf_procedures->amf_common_procs, wrapper, entries);
    return ident_proc;
  } else {
    free_wrapper((void**) &ident_proc);
  }
  return ident_proc;
}

/***************************************************************************
** Description: Sends IDENTITY REQUEST message.                           **
**                                                                        **
** Inputs:  args:      handler parameters                                 **
**      Others:    None                                                   **
**                                                                        **
** Outputs:        None                                                   **
**      Return:    None                                                   **
***************************************************************************/
static int amf_identification_request(nas_amf_ident_proc_t* const proc) {
  OAILOG_FUNC_IN(LOG_NAS_EMM);
  amf_sap_t amf_sap = {};
  int rc            = RETURNok;
  proc->T3570.id    = NAS5G_TIMER_INACTIVE_ID;
  OAILOG_DEBUG(LOG_AMF_APP, "Sending AS IDENTITY_REQUEST\n");
  /*
   * Notify AMF-AS SAP that Identity Request message has to be sent
   * to the UE
   */
  amf_sap.primitive                                    = AMFAS_SECURITY_REQ;
  amf_sap.u.amf_as.u.security.ue_id                    = proc->ue_id;
  amf_sap.u.amf_as.u.security.msg_type                 = AMF_AS_MSG_TYPE_IDENT;
  amf_sap.u.amf_as.u.security.ident_type               = proc->identity_type;
  amf_sap.u.amf_as.u.security.sctx.is_knas_int_present = true;
  amf_sap.u.amf_as.u.security.sctx.is_knas_enc_present = true;
  amf_sap.u.amf_as.u.security.sctx.is_new              = true;
  rc                                                   = amf_sap_send(&amf_sap);

  if (rc != RETURNerror) {
    /*
     * Start Identification T3570 timer
     */
    OAILOG_DEBUG(
        LOG_AMF_APP, "AMF_TEST: Timer: Starting Identity timer T3570 \n");
    proc->T3570.id = amf_app_start_timer(
        IDENTITY_TIMER_EXPIRY_MSECS, TIMER_REPEAT_ONCE,
        identification_t3570_handler, proc->ue_id);
  }
  OAILOG_FUNC_RETURN(LOG_NAS_AMF, rc);
}

/* Identification Timer T3570 Expiry Handler */
static int identification_t3570_handler(
    zloop_t* loop, int timer_id, void* arg) {
  amf_ue_ngap_id_t ue_id = 0;
  amf_context_t* amf_ctx = NULL;
  OAILOG_FUNC_IN(LOG_NAS_AMF);

  if (!amf_pop_timer_arg(timer_id, &ue_id)) {
    OAILOG_WARNING(
        LOG_AMF_APP, "T3570: Invalid Timer Id expiration, timer Id: %u\n",
        timer_id);
    OAILOG_FUNC_RETURN(LOG_NAS_AMF, RETURNok);
  }

  ue_m5gmm_context_s* ue_amf_context =
      amf_ue_context_exists_amf_ue_ngap_id(ue_id);

  if (ue_amf_context == NULL) {
    OAILOG_DEBUG(
        LOG_AMF_APP,
        "T3570: ue_amf_context is NULL for UE ID: " AMF_UE_NGAP_ID_FMT, ue_id);
    OAILOG_FUNC_RETURN(LOG_NAS_AMF, RETURNok);
  }

  amf_ctx = &ue_amf_context->amf_context;
  if (!(amf_ctx)) {
    OAILOG_ERROR(
        LOG_AMF_APP,
        "T3570: timer expired No AMF context for UE ID: " AMF_UE_NGAP_ID_FMT,
        ue_id);
    OAILOG_FUNC_RETURN(LOG_NAS_AMF, RETURNok);
  }

  nas_amf_ident_proc_t* ident_proc =
      get_5g_nas_common_procedure_identification(amf_ctx);

  if (ident_proc) {
    OAILOG_WARNING(
        LOG_AMF_APP,
        "T3570: Timer expired for timer id %lu for UE ID " AMF_UE_NGAP_ID_FMT,
        ident_proc->T3570.id, ident_proc->ue_id);
    ident_proc->T3570.id = NAS5G_TIMER_INACTIVE_ID;
    /*
     * Increment the retransmission counter
     */
    ident_proc->retransmission_count += 1;
    OAILOG_ERROR(
        LOG_AMF_APP, "T3570: Incrementing retransmission_count to %d\n",
        ident_proc->retransmission_count);

    if (ident_proc->retransmission_count < IDENTIFICATION_COUNTER_MAX) {
      /*
       * Send identity request message to the UE
       */
      OAILOG_ERROR(
          LOG_AMF_APP,
          "T3570: timer has expired retransmitting Identification request \n");
      amf_identification_request(ident_proc);
    } else {
      /*
       * Abort the identification procedure
       */
      OAILOG_ERROR(
          LOG_AMF_APP,
          "T3570: Maximum retires:%d, done hence Abort the "
          "identification "
          "procedure\n",
          ident_proc->retransmission_count);
      amf_proc_registration_abort(amf_ctx, ue_amf_context);
    }
  }
  OAILOG_FUNC_RETURN(LOG_NAS_AMF, RETURNok);
}

//-------------------------------------------------------------------------------------
int amf_proc_identification(
    amf_context_t* const amf_context, nas_amf_proc_t* const amf_proc,
    const identity_type2_t type, success_cb_t success, failure_cb_t failure) {
  OAILOG_FUNC_IN(LOG_NAS_AMF);
  int rc                     = RETURNerror;
  amf_context->amf_fsm_state = AMF_REGISTERED;
  if ((amf_context) && ((AMF_DEREGISTERED == amf_context->amf_fsm_state) ||
                        (AMF_REGISTERED == amf_context->amf_fsm_state))) {
    amf_ue_ngap_id_t ue_id =
        PARENT_STRUCT(amf_context, ue_m5gmm_context_s, amf_context)
            ->amf_ue_ngap_id;
    nas_amf_ident_proc_t* ident_proc =
        nas5g_new_identification_procedure(amf_context);
    if (ident_proc) {
      if (amf_proc) {
        if ((NAS_AMF_PROC_TYPE_SPECIFIC == amf_proc->type) &&
            (AMF_SPEC_PROC_TYPE_REGISTRATION ==
             ((nas_amf_specific_proc_t*) amf_proc)->type)) {
          ident_proc->is_cause_is_registered = true;
        }
      }
      ident_proc->identity_type                                 = type;
      ident_proc->retransmission_count                          = 0;
      ident_proc->ue_id                                         = ue_id;
      ident_proc->amf_com_proc.amf_proc.delivered               = NULL;
      ident_proc->amf_com_proc.amf_proc.base_proc.success_notif = success;
      ident_proc->amf_com_proc.amf_proc.base_proc.failure_notif = failure;
      ident_proc->amf_com_proc.amf_proc.base_proc.fail_in       = NULL;
    }
    rc = amf_identification_request(ident_proc);

    if (rc != RETURNerror) {
      /*
       * Notify 5G CN that common procedure has been initiated
       */
      amf_sap_t amf_sap       = {};
      amf_sap.primitive       = AMFREG_COMMON_PROC_REQ;
      amf_sap.u.amf_reg.ue_id = ue_id;
      amf_sap.u.amf_reg.ctx   = amf_context;
      rc                      = amf_sap_send(&amf_sap);
    }
  }
  OAILOG_FUNC_RETURN(LOG_NAS_AMF, rc);
}

int amf_nas_proc_auth_param_res(
    amf_ue_ngap_id_t amf_ue_ngap_id, uint8_t nb_vectors,
    m5gauth_vector_t* vectors) {
  OAILOG_FUNC_IN(LOG_AMF_APP);

  int rc                            = RETURNerror;
  amf_sap_t amf_sap                 = {};
  amf_cn_auth_res_t amf_cn_auth_res = {};

  amf_cn_auth_res.ue_id      = amf_ue_ngap_id;
  amf_cn_auth_res.nb_vectors = nb_vectors;
  for (int i = 0; i < nb_vectors; i++) {
    amf_cn_auth_res.vector[i] = &vectors[i];
  }

  amf_sap.primitive           = AMFCN_AUTHENTICATION_PARAM_RES;
  amf_sap.u.amf_cn.u.auth_res = &amf_cn_auth_res;
  rc                          = amf_sap_send(&amf_sap);

  OAILOG_FUNC_RETURN(LOG_NAS_AMF, rc);
}

int amf_nas_proc_authentication_info_answer(
    itti_amf_subs_auth_info_ans_t* aia) {
  imsi64_t imsi64                       = INVALID_IMSI64;
  int rc                                = RETURNerror;
  amf_context_t* amf_ctxt_p             = NULL;
  ue_m5gmm_context_s* ue_5gmm_context_p = NULL;
  int amf_cause                         = -1;
  OAILOG_FUNC_IN(LOG_AMF_APP);

  IMSI_STRING_TO_IMSI64((char*) aia->imsi, &imsi64);

  OAILOG_DEBUG(LOG_AMF_APP, "Handling imsi " IMSI_64_FMT "\n", imsi64);

  ue_5gmm_context_p = lookup_ue_ctxt_by_imsi(imsi64);

  if (ue_5gmm_context_p) {
    amf_ctxt_p = &ue_5gmm_context_p->amf_context;
  }

  if (!(amf_ctxt_p)) {
    OAILOG_ERROR(
        LOG_NAS_AMF, "That's embarrassing as we don't know this IMSI\n");
    OAILOG_FUNC_RETURN(LOG_NAS_AMF, RETURNerror);
  }

  amf_ue_ngap_id_t amf_ue_ngap_id = ue_5gmm_context_p->amf_ue_ngap_id;

  OAILOG_DEBUG(
      LOG_NAS_AMF,
      "Received Authentication Information Answer from Subscriberdb for"
      " UE ID = " AMF_UE_NGAP_ID_FMT,
      amf_ue_ngap_id);

  if (aia->auth_info.nb_of_vectors) {
    /*
     * Check that list is not empty and contain at most MAX_EPS_AUTH_VECTORS
     * elements
     */
    if (aia->auth_info.nb_of_vectors > MAX_EPS_AUTH_VECTORS) {
      OAILOG_WARNING(
          LOG_NAS_AMF,
          "nb_of_vectors should be lesser than max_eps_auth_vectors");
      return RETURNerror;
    }

    OAILOG_DEBUG(
        LOG_NAS_AMF, "INFORMING NAS ABOUT AUTH RESP SUCCESS got %u vector(s)\n",
        aia->auth_info.nb_of_vectors);
    rc = amf_nas_proc_auth_param_res(
        amf_ue_ngap_id, aia->auth_info.nb_of_vectors,
        aia->auth_info.m5gauth_vector);
  } else {
    OAILOG_ERROR(
        LOG_NAS_AMF, "nb_of_vectors received is zero from subscriberdb");
    amf_cause = AMF_UE_ILLEGAL;
    rc        = amf_proc_registration_reject(amf_ue_ngap_id, amf_cause);
    OAILOG_FUNC_RETURN(LOG_NAS_AMF, rc);
  }

  OAILOG_FUNC_RETURN(LOG_NAS_AMF, rc);
}

int amf_decrypt_imsi_info_answer(itti_amf_decrypted_imsi_info_ans_t* aia) {
  imsi64_t imsi64                = INVALID_IMSI64;
  int rc                         = RETURNerror;
  amf_context_t* amf_ctxt_p      = NULL;
  ue_m5gmm_context_s* ue_context = NULL;
  int amf_cause                  = -1;

  // Local imsi to be put in imsi defined in 3gpp_23.003.h
  supi_as_imsi_t supi_imsi;
  amf_guti_m5g_t amf_guti;
  guti_and_amf_id_t guti_and_amf_id;
  const bool is_amf_ctx_new = true;
  OAILOG_FUNC_IN(LOG_AMF_APP);

  ue_context = amf_ue_context_exists_amf_ue_ngap_id(aia->ue_id);

  IMSI_STRING_TO_IMSI64((char*) aia->imsi, &imsi64);

  OAILOG_DEBUG(LOG_AMF_APP, "Handling imsi " IMSI_64_FMT "\n", imsi64);

  if (ue_context) {
    amf_ctxt_p = &ue_context->amf_context;
  }

  if (!(amf_ctxt_p)) {
    OAILOG_ERROR(
        LOG_NAS_AMF, "That's embarrassing as we don't know this IMSI\n");
    OAILOG_FUNC_RETURN(LOG_NAS_AMF, RETURNerror);
  }

  amf_ue_ngap_id_t amf_ue_ngap_id = ue_context->amf_ue_ngap_id;

  OAILOG_DEBUG(
      LOG_NAS_AMF,
      "Received decrypted imsi Information Answer from Subscriberdb for"
      " UE ID = " AMF_UE_NGAP_ID_FMT,
      amf_ue_ngap_id);

  amf_registration_request_ies_t* params =
      new (amf_registration_request_ies_t)();

  params->imsi = new imsi_t();

  supi_imsi.plmn.mcc_digit1 =
      ue_context->amf_context.m5_guti.guamfi.plmn.mcc_digit1;
  supi_imsi.plmn.mcc_digit2 =
      ue_context->amf_context.m5_guti.guamfi.plmn.mcc_digit2;
  supi_imsi.plmn.mcc_digit3 =
      ue_context->amf_context.m5_guti.guamfi.plmn.mcc_digit3;
  supi_imsi.plmn.mnc_digit1 =
      ue_context->amf_context.m5_guti.guamfi.plmn.mnc_digit1;
  supi_imsi.plmn.mnc_digit2 =
      ue_context->amf_context.m5_guti.guamfi.plmn.mnc_digit2;
  supi_imsi.plmn.mnc_digit3 =
      ue_context->amf_context.m5_guti.guamfi.plmn.mnc_digit3;

  memcpy(&supi_imsi.msin, aia->imsi, MSIN_MAX_LENGTH);

  // Copy entire supi_imsi to param->imsi->u.value
  memcpy(&params->imsi->u.value, &supi_imsi, IMSI_BCD8_SIZE);

  if (supi_imsi.plmn.mnc_digit3 != 0xf) {
    params->imsi->u.value[0] = ((supi_imsi.plmn.mcc_digit1 << 4) & 0xf0) |
                               (supi_imsi.plmn.mcc_digit2 & 0xf);
    params->imsi->u.value[1] = ((supi_imsi.plmn.mcc_digit3 << 4) & 0xf0) |
                               (supi_imsi.plmn.mnc_digit1 & 0xf);
    params->imsi->u.value[2] = ((supi_imsi.plmn.mnc_digit2 << 4) & 0xf0) |
                               (supi_imsi.plmn.mnc_digit3 & 0xf);
  }

  amf_app_generate_guti_on_supi(&amf_guti, &supi_imsi);
  amf_ue_context_on_new_guti(
      ue_context, reinterpret_cast<guti_m5_t*>(&amf_guti));

  ue_context->amf_context.m5_guti.m_tmsi = amf_guti.m_tmsi;
  ue_context->amf_context.m5_guti.guamfi = amf_guti.guamfi;
  imsi64                                 = amf_imsi_to_imsi64(params->imsi);
  guti_and_amf_id.amf_guti               = amf_guti;
  guti_and_amf_id.amf_ue_ngap_id         = aia->ue_id;

  if (amf_supi_guti_map.size() == 0) {
    // first entry.
    amf_supi_guti_map.insert(
        std::pair<imsi64_t, guti_and_amf_id_t>(imsi64, guti_and_amf_id));
  } else {
    /* already elements exist then check if same imsi already present
     * if same imsi then update/overwrite the element
     */
    std::unordered_map<imsi64_t, guti_and_amf_id_t>::iterator found_imsi =
        amf_supi_guti_map.find(imsi64);
    if (found_imsi == amf_supi_guti_map.end()) {
      // it is new entry to map
      amf_supi_guti_map.insert(
          std::pair<imsi64_t, guti_and_amf_id_t>(imsi64, guti_and_amf_id));
    } else {
      // Overwrite the second element.
      found_imsi->second = guti_and_amf_id;
    }
  }

  params->decode_status = ue_context->amf_context.decode_status;
  /*
   * Execute the requested new UE registration procedure
   * This will initiate identity req in DL.
   */
  rc = amf_proc_registration_request(aia->ue_id, is_amf_ctx_new, params);
  OAILOG_FUNC_RETURN(LOG_NAS_AMF, rc);
}

int amf_handle_s6a_update_location_ans(
    const s6a_update_location_ans_t* ula_pP) {
  imsi64_t imsi64                   = INVALID_IMSI64;
  amf_context_t* amf_ctxt_p         = NULL;
  ue_m5gmm_context_s* ue_mm_context = NULL;
  OAILOG_FUNC_IN(LOG_AMF_APP);

  IMSI_STRING_TO_IMSI64((char*) ula_pP->imsi, &imsi64);

  ue_mm_context = lookup_ue_ctxt_by_imsi(imsi64);

  if (ue_mm_context) {
    amf_ctxt_p = &ue_mm_context->amf_context;
  }

  if (!(amf_ctxt_p)) {
    OAILOG_ERROR(LOG_NAS_AMF, "IMSI is invalid\n");
    OAILOG_FUNC_RETURN(LOG_NAS_AMF, RETURNerror);
  }

  amf_ue_ngap_id_t amf_ue_ngap_id = ue_mm_context->amf_ue_ngap_id;

  // Validating whether the apn_config sent from ue and saved in amf_ctx is
  // present in s6a_update_location_ans_t received from subscriberdb.
  memcpy(
      &amf_ctxt_p->apn_config_profile,
      &ula_pP->subscription_data.apn_config_profile,
      sizeof(apn_config_profile_t));
  OAILOG_DEBUG(
      LOG_NAS_AMF,
      "Received update location Answer from Subscriberdb for"
      " ue_id = " AMF_UE_NGAP_ID_FMT,
      amf_ue_ngap_id);
  memcpy(
      &amf_ctxt_p->subscribed_ue_ambr,
      &ula_pP->subscription_data.subscribed_ambr, sizeof(ambr_t));

  OAILOG_DEBUG(
      LOG_NAS_AMF,
      "Received UL rate %" PRIu64 " and DL rate %" PRIu64 "and BR unit: %d \n",
      ula_pP->subscription_data.subscribed_ambr.br_ul,
      ula_pP->subscription_data.subscribed_ambr.br_dl,
      ula_pP->subscription_data.subscribed_ambr.br_unit);

  OAILOG_FUNC_RETURN(LOG_NAS_AMF, RETURNok);
}

/* Cleanup all procedures in amf_context */
void amf_nas_proc_clean_up(ue_m5gmm_context_s* ue_context_p) {
  // Delete registration procedures
  amf_delete_registration_proc(&(ue_context_p->amf_context));
}

void nas_amf_procedure_gc(amf_context_t* const amf_context) {
  if (LIST_EMPTY(&amf_context->amf_procedures->amf_common_procs) &&
      LIST_EMPTY(&amf_context->amf_procedures->cn_procs) &&
      (!amf_context->amf_procedures->amf_specific_proc)) {
    delete amf_context->amf_procedures;
    amf_context->amf_procedures = nullptr;
  }
}

}  // namespace magma5g
