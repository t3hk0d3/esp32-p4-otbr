/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
otDeviceRole mock_role;
otCommissionerState mock_commissioner;
otError mock_dataset_error, mock_pending_error, mock_pending_result;
otOperationalDataset mock_dataset;
otOperationalDatasetTlvs mock_active_tlvs, mock_update_tlvs, mock_convert_tlvs;
otError mock_set_active_error, mock_thread_enable_error, mock_clear_pending_error;
char mock_calls[32];
void mock_call(char call) {
    size_t length=strlen(mock_calls);
    if(length+1<sizeof(mock_calls)) {mock_calls[length]=call;mock_calls[length+1]=0;}
}
static otDatasetMgmtSetCallback pending_callback;
static void *pending_context;
static otReceiveDiagnosticGetCallback diagnostic_callback;
static void *diagnostic_context;
void mock_reset(void) {
    mock_role=OT_DEVICE_ROLE_DISABLED;
    mock_commissioner=OT_COMMISSIONER_STATE_DISABLED;
    mock_dataset_error=OT_ERROR_NONE;
    mock_pending_error=OT_ERROR_NOT_FOUND;
    mock_pending_result=OT_ERROR_NONE;
    memset(&mock_dataset,0,sizeof(mock_dataset));
    mock_dataset.mComponents.mIsNetworkNamePresent=true;
    strcpy(mock_dataset.mNetworkName.m8,"test-mesh");
    memset(&mock_active_tlvs,0,sizeof(mock_active_tlvs));
    mock_active_tlvs.mLength=2;mock_active_tlvs.mTlvs[0]=0xab;mock_active_tlvs.mTlvs[1]=0xcd;
    memset(&mock_update_tlvs,0,sizeof(mock_update_tlvs));
    memset(&mock_convert_tlvs,0,sizeof(mock_convert_tlvs));
    mock_set_active_error=OT_ERROR_NONE;
    mock_thread_enable_error=OT_ERROR_NONE;
    mock_clear_pending_error=OT_ERROR_NONE;
    mock_calls[0]=0;
    pending_callback=NULL;
    diagnostic_callback=NULL;
    mock_netdiag_sends=0;
    mock_netdiag_filtered_sends=0;
    mock_time=0;
    mock_ticks=0;
    mock_on_delay=NULL;
    mock_diag_tlv_count=0;
    mock_mode=BR_MODE_BR_RUNNING;
    mock_ot_ready=true;
    mock_ot_writes=0;
    mock_on_lock=NULL;
    mock_session_refused=false;
}
void mock_complete_pending(void) {
    if(pending_callback) {otDatasetMgmtSetCallback callback=pending_callback;pending_callback=NULL;callback(mock_pending_result,pending_context);}
}
void mock_deliver_diagnostic(uintptr_t token) {
    otMessageInfo info={0};
    if(diagnostic_callback) {diagnostic_callback(OT_ERROR_NONE,(otMessage *)(uintptr_t)1,&info,(void *)(token?token:(uintptr_t)diagnostic_context));}
}
otError mock_next_diag_tlv(otNetworkDiagIterator *iterator, otNetworkDiagTlv *tlv) {
    if(*iterator>=mock_diag_tlv_count) {return OT_ERROR_NOT_FOUND;}
    *tlv=mock_diag_tlvs[(*iterator)++];
    return OT_ERROR_NONE;
}
