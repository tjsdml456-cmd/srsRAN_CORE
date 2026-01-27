# Dynamic 5QI and GBR Modification 기능 구현 가이드

## 개요

이 문서는 Open5GS SMF에 동적 5QI 및 GBR(Guaranteed Bit Rate)/MBR(Maximum Bit Rate) 수정 기능을 추가하고, srsRAN gNB의 스케줄러까지 QoS 파라미터가 전달되도록 구현한 전체 과정을 설명합니다.

**구현 목표:**
- Open5GS SMF에서 REST API를 통해 실시간으로 QoS 파라미터(5QI, GBR, MBR) 변경
- 변경된 QoS 파라미터가 NGAP → CU-CP → DU → 스케줄러까지 전달
- 스케줄러에서 GBR weight, delay weight를 계산하여 QoS 보장

**srsRAN 스케줄러 연동:**
- non-GBR QoS Flow: MBR 값을 srsRAN 스케줄러의 `dl_gbr`로 사용 (기본값: 5Mbps)
- delay_critical GBR QoS Flow: GBR 값을 srsRAN 스케줄러의 `dl_gbr`로 사용 (기본값: 25Mbps)

---

## 구현 타임라인

### Phase 1: Open5GS SMF 커스텀 API 구현 (초기 단계)

**목표**: SMF에 동적 QoS 수정 API 추가

1. **`src/smf/qos-modify.h` / `qos-modify.c` 생성**
   - 커스텀 API 핸들러 구현
   - JSON 요청 파싱 (supi, psi, qfi, 5qi, gbr_dl, gbr_ul, mbr_dl, mbr_ul)
   - UE/세션/QoS Flow 찾기
   - QoS 파라미터 업데이트
   - PFCP 및 NGAP 메시지 전송

2. **`src/smf/smf-sm.c` 수정**
   - SBI 라우팅에 `/qos-modify` 엔드포인트 추가
   - POST 메서드 처리

3. **`src/smf/meson.build` 수정**
   - 새 파일을 빌드 시스템에 추가

4. **`lib/sbi/message.c` 수정**
   - `qos-modify` 리소스 이름 허용 (Unknown resource name 에러 방지)

**결과**: API 호출은 성공했으나, NGAP 메시지에 GBR 정보가 포함되지 않음

---

### Phase 2: NGAP 메시지에 GBR 정보 포함 (Open5GS SMF)

**문제**: GBR QoS Flow인데 NGAP 메시지에 GBR QoS Information이 포함되지 않음

**원인**: 
- `ngap-build.c`의 `fill_qos_level_parameters()` 함수에서 GBR 정보 포함 조건이 너무 엄격함
- MBR이 명시적으로 제공되지 않으면 0으로 남아있어 GBR 정보가 포함되지 않음

**수정 사항**:

1. **`src/smf/ngap-build.c` 수정**
   ```c
   // 기존: if (include_gbr && qos->gbr.downlink && qos->gbr.uplink)
   // 변경: 한 방향만 있어도 GBR 정보 포함
   if (include_gbr && (qos->gbr.downlink > 0 || qos->gbr.uplink > 0)) {
       // MBR이 없으면 GBR 값을 MBR로 사용
       uint64_t mbr_dl = qos->mbr.downlink > 0 ? qos->mbr.downlink : gbr_dl;
       uint64_t mbr_ul = qos->mbr.uplink > 0 ? qos->mbr.uplink : gbr_ul;
       // GBR QoS Information 포함
   }
   ```

2. **`src/smf/qos-modify.c` 수정**
   - GBR이 제공되고 MBR이 없는 경우, GBR 값을 MBR로 자동 설정
   - PFCP 플래그 수정: `OGS_PFCP_MODIFY_SESSION | OGS_PFCP_MODIFY_NETWORK_REQUESTED | OGS_PFCP_MODIFY_QOS_MODIFY`

**결과**: NGAP 메시지에 GBR 정보가 포함됨을 확인

---

### Phase 3: srsRAN gNB NGAP 파싱 (초기)

**문제**: Open5GS SMF가 NGAP 메시지에 GBR 정보를 포함했지만, srsRAN gNB가 파싱하지 않음

**수정 사항**:

1. **`lib/ngap/ngap_asn1_helpers.h` 수정**
   - `PDU Session Resource Modify Request Transfer` 파싱 시 GBR QoS Information 추출
   - `qos_flow_setup_request_item`에 GBR 정보 설정

**결과**: NGAP에서 GBR 정보 파싱 성공

---

### Phase 4: CU-CP → DU QoS 정보 전달 (핵심 문제)

**문제**: CU-CP에서 QoS 정보를 계산했지만 DU로 전달되지 않음

**원인 분석**:
1. `pdu_session_resource_modification_routine.cpp`에서 F1AP 메시지 생성 시 QoS 정보 미포함
2. `f1ap_drb_to_modify` 구조체에 QoS 정보 필드 없음
3. F1AP ASN.1 인코딩/디코딩에 QoS 정보 미포함
4. DU의 `modify_drbs()` 함수가 QoS 정보를 읽지 않음

**수정 사항**:

1. **`include/srsran/f1ap/ue_context_management_configs.h` 수정**
   ```cpp
   struct f1ap_drb_to_modify {
       drb_id_t drb_id;
       std::vector<up_transport_layer_info> uluptnl_info_list;
       std::optional<f1ap_drb_info> qos_info;  // 추가됨
   };
   ```

2. **`lib/cu_cp/routines/pdu_session_resource_modification_routine.cpp` 수정**
   ```cpp
   // F1AP DRB modify item 생성 시 QoS 정보 포함
   f1ap_drb_info qos_info;
   qos_info.drb_qos = drb_to_modify.qos_params;
   qos_info.s_nssai = drb_to_modify.s_nssai;
   // QoS flows 정보 포함
   for (const auto& flow : drb_to_modify.qos_flows) {
       flow_mapped_to_drb flow_item;
       flow_item.qos_flow_id = flow.first;
       flow_item.qos_flow_level_qos_params = flow.second.qos_params;
       qos_info.flows_mapped_to_drb_list.push_back(flow_item);
   }
   drb_modify_item.qos_info = qos_info;
   ```

3. **`lib/f1ap/asn1_helpers.cpp` 수정**
   - `make_drb_to_mod()`: QoS 정보 인코딩 추가
   - `make_drb_to_modify()`: QoS 정보 디코딩 추가

4. **`lib/du/du_high/du_manager/ran_resource_management/du_bearer_resource_manager.cpp` 수정**
   ```cpp
   // modify_drbs() 함수에서 QoS 정보 업데이트
   if (drb_to_modify.qos_info.has_value()) {
       existing_drb.qos = drb_to_modify.qos_info.value().drb_qos;
       existing_drb.s_nssai = drb_to_modify.qos_info.value().s_nssai;
       // 로깅 추가
   }
   ```

**결과**: CU-CP에서 DU로 QoS 정보 전달 성공

---

### Phase 5: CU-CP UP Resource Manager 수정

**문제**: UP Resource Manager가 DRB 수정 요청을 처리하지 않음

**수정 사항**:

1. **`lib/cu_cp/up_resource_manager/up_resource_manager_impl.cpp` 수정**
   ```cpp
   // apply_update_for_modified_drbs() 헬퍼 함수 추가
   static void apply_update_for_modified_drbs(
       up_pdu_session_context& pdu_session_context,
       up_context& context,
       const std::map<drb_id_t, up_drb_context>& drb_to_modify)
   {
       for (const auto& drb : drb_to_modify) {
           auto& existing_drb = pdu_session_context.drbs.at(drb.first);
           // QoS 파라미터 업데이트
           existing_drb.qos_params = drb.second.qos_params;
           // QoS flows 업데이트
           for (const auto& flow : drb.second.qos_flows) {
               existing_drb.qos_flows[flow.first] = flow.second;
               context.qos_flow_map[flow.first] = drb.first;
           }
           // 기타 DRB 파라미터 업데이트
           existing_drb.pdcp_cfg = drb.second.pdcp_cfg;
           existing_drb.sdap_cfg = drb.second.sdap_cfg;
           existing_drb.rlc_mod = drb.second.rlc_mod;
           context.drb_dirty[get_dirty_drb_index(drb.first)] = true;
       }
   }
   
   // apply_config_update()에서 호출
   for (const auto& mod_session : result.pdu_sessions_modified_list) {
       apply_update_for_modified_drbs(session_context, context, mod_session.drb_to_modify);
   }
   ```

2. **`lib/cu_cp/up_resource_manager/up_resource_manager_helpers.cpp` 수정**
   ```cpp
   // modify_qos_flow() 함수 수정: 기존 DRB 정보 보존
   static drb_id_t modify_qos_flow(...) {
       // 기존 DRB 컨텍스트 가져오기
       const auto& existing_drb = full_context.pdu_sessions.at(new_session_context.id)
                                     .drbs.at(drb_id);
       // 기존 DRB 정보로 시작
       up_drb_context drb_ctx = existing_drb;
       // QoS 파라미터만 업데이트
       drb_ctx.qos_params = qos_flow.qos_flow_level_qos_params;
       // 수정된 DRB 추가
       new_session_context.drb_to_modify.emplace(drb_id, drb_ctx);
   }
   ```

**결과**: UP Resource Manager가 DRB 수정 요청을 올바르게 처리

---

### Phase 6: 스케줄러에 QoS 정보 전달

**문제**: DU에서 QoS 정보를 업데이트했지만 스케줄러에 전달되지 않음

**수정 사항**:

1. **`lib/du/du_high/du_manager/converters/scheduler_configuration_helpers.cpp` 확인**
   - DRB QoS 정보를 Logical Channel 설정에 반영하는 로직 확인
   - GBR 정보가 있으면 `lc_cfg.qos->gbr_qos_info` 설정

**결과**: 스케줄러에 QoS 정보 전달 확인

---

### Phase 7: 스케줄러 QoS 가중치 계산 및 로깅

**목표**: GBR weight, delay weight 계산 및 디버깅을 위한 로깅 추가

**수정 사항**:

1. **`lib/scheduler/policy/scheduler_time_qos.cpp` 수정**
   ```cpp
   // GBR weight 계산
   if (lc->qos->gbr_qos_info.has_value()) {
       double dl_avg_rate = u.dl_avg_bit_rate(lc->lcid);
       if (dl_avg_rate != 0) {
           double calculated_gbr_weight = 
               lc->qos->gbr_qos_info->gbr_dl / dl_avg_rate;
           gbr_weight += std::min(calculated_gbr_weight, max_metric_weight);
           // 로깅 (100번마다)
           logger.info("GBR weight calculation: LCID={}, GBR_DL={} bps, "
                      "avg_rate={} bps, calculated_gbr_weight={:.6f}, "
                      "final_gbr_weight={:.6f}",
                      fmt::underlying(lc->lcid),
                      lc->qos->gbr_qos_info->gbr_dl,
                      dl_avg_rate,
                      calculated_gbr_weight,
                      gbr_weight);
       }
   }
   
   // Priority weight 로깅 (100번마다)
   logger.info("DL Priority calc: UE{} min_combined_prio={}, prio_weight={:.3f}, "
              "pf_weight={:.3f}, gbr_weight={:.3f}, delay_weight={:.3f}",
              u.ue_index(), min_combined_prio, prio_weight, pf_weight, 
              gbr_weight, delay_weight);
   ```

2. **`lib/scheduler/logging/scheduler_metrics_handler.cpp` 수정**
   ```cpp
   // Throughput 로깅 추가
   logger.info("UE{} Throughput calc: sum_dl_tb_bytes={}, period={}ms, "
              "dl_brate_kbps={:.2f} (={:.2f}Mbps), dl_nof_ok={}, "
              "ul_brate_kbps={:.2f} (={:.2f}Mbps), ul_nof_ok={}",
              ue_index, data.sum_dl_tb_bytes, metric_report_period.count(),
              ret.dl_brate_kbps, ret.dl_brate_kbps / 1000.0, ret.dl_nof_ok,
              ret.ul_brate_kbps, ret.ul_brate_kbps / 1000.0, ret.ul_nof_ok);
   ```

**결과**: 스케줄러에서 QoS 가중치 계산 및 로깅 확인

---

### Phase 8: RLC → MAC → Scheduler HOL TOA 전달 (Delay Weight 계산)

**문제**: `delay_weight`가 항상 1.0으로 계산됨 (`hol_toa_valid=false`)

**원인**: 
- RLC에서 계산한 HOL TOA가 `steady_clock` 타입
- MAC/Scheduler는 `system_clock` 타입을 기대
- 타입 불일치로 HOL TOA가 전달되지 않음

**수정 사항**:

1. **`include/srsran/rlc/rlc_buffer_state.h` 수정**
   ```cpp
   struct rlc_buffer_state {
       unsigned pending_bytes = 0;
       // steady_clock에서 system_clock로 변경
       std::optional<std::chrono::system_clock::time_point> hol_toa;
   };
   ```

2. **`lib/rlc/rlc_tx_am_entity.cpp` 수정**
   ```cpp
   // steady_clock을 system_clock으로 변환하는 헬퍼 함수 추가
   namespace {
   std::optional<std::chrono::system_clock::time_point>
   convert_steady_to_system(const std::optional<std::chrono::steady_clock::time_point>& steady_tp)
   {
       if (not steady_tp.has_value()) {
           return std::nullopt;
       }
       auto now_steady = std::chrono::steady_clock::now();
       auto delay = now_steady - steady_tp.value();
       return std::chrono::system_clock::now() - 
              std::chrono::duration_cast<std::chrono::system_clock::duration>(delay);
   }
   }
   
   // get_buffer_state()에서 변환 적용
   bs.hol_toa = convert_steady_to_system(sdu_info.time_of_arrival);
   // 또는
   bs.hol_toa = convert_steady_to_system(next_sdu->time_of_arrival);
   ```

3. **`lib/rlc/rlc_tx_um_entity.cpp` 수정**
   - 동일한 변환 로직 추가

4. **`lib/rlc/rlc_tx_tm_entity.cpp` 수정**
   - 동일한 변환 로직 추가

5. **`lib/mac/mac_dl/mac_cell_processor.cpp` 수정**
   ```cpp
   rlc_buffer_state rlc_bs = bearer->on_buffer_state_update();
   mac_dl_buffer_state_indication_message bs{
       ue_mng.get_ue_index(grant.pdsch_cfg.rnti), 
       lc_info.lcid.to_lcid(), 
       rlc_bs.pending_bytes};
   // hol_toa 전달
   if (rlc_bs.hol_toa.has_value()) {
       bs.hol_toa = rlc_bs.hol_toa.value();
   }
   sched.handle_dl_buffer_state_update(bs);
   ```

**결과**: HOL TOA가 Scheduler까지 전달되어 `delay_weight` 계산 성공

---

### Phase 9: Python 분석 스크립트 개발

**목표**: 로그에서 QoS 메트릭 추출 및 분석

**생성된 파일**:

1. **`misc/extract_ue_priority.py`**
   - Priority 정보 추출 (min_combined_prio, prio_weight, pf_weight, gbr_weight, delay_weight)
   - 로그 패턴: `DL Priority calc: UE{} ...`

2. **`misc/extract_ue_throughput.py`**
   - Throughput 정보 추출 (DL/UL bitrate)
   - 로그 패턴: `UE{} Throughput calc: ...`

3. **`misc/extract_ue_hol_delay.py`**
   - HOL Delay 정보 추출 (큐잉 딜레이)
   - 로그 패턴: `[DELAY-WEIGHT] UE{} ...`
   - 시스템 시간과 hol_delay_ms만 출력

---

## 전체 데이터 흐름

```
[Open5GS SMF]
    ↓ POST /nsmf-pdusession/v1/qos-modify
    ↓ JSON 파싱: {supi, psi, qfi, 5qi, gbr_dl, gbr_ul, mbr_dl, mbr_ul}
    ↓ QoS Flow 찾기 및 파라미터 업데이트
    ↓
    ├─→ PFCP: UPF에 QoS Flow Modification Request
    │   └─→ OGS_PFCP_MODIFY_SESSION | OGS_PFCP_MODIFY_NETWORK_REQUESTED | OGS_PFCP_MODIFY_QOS_MODIFY
    │
    └─→ NGAP: gNB에 PDU Session Resource Modify Request
        └─→ GBR QoS Information 포함 (GBR_DL, GBR_UL, MBR_DL, MBR_UL)
        ↓
[srsRAN gNB - NGAP]
    ↓ NGAP 파싱: GBR QoS Information 추출
    ↓ qos_flow_setup_request_item에 GBR 정보 설정
    ↓
[srsRAN gNB - CU-CP]
    ↓ PDU Session Resource Modification Routine
    ↓ UP Resource Manager: calculate_update()
    ↓ modify_qos_flow(): 기존 DRB 정보 보존하며 QoS만 업데이트
    ↓ E1AP: Bearer Context Modification Request
    ↓
[srsRAN gNB - CU-UP]
    ↓ E1AP 응답 처리
    ↓
[srsRAN gNB - CU-CP]
    ↓ E1AP 응답 수신
    ↓ F1AP: UE Context Modification Request 생성
    ↓ f1ap_drb_to_modify에 qos_info 포함
    ↓ F1AP ASN.1 인코딩
    ↓
[srsRAN gNB - DU]
    ↓ F1AP 파싱: qos_info 디코딩
    ↓ du_bearer_resource_manager::modify_drbs()
    ↓ DRB QoS 파라미터 업데이트 (existing_drb.qos = drb_to_modify.qos_info.value().drb_qos)
    ↓ scheduler_configuration_helpers: Logical Channel QoS Config 설정
    ↓
[srsRAN 스케줄러]
    ↓ Logical Channel에 QoS 정보 반영
    ↓ compute_dl_qos_weights():
    │   ├─→ prio_weight = (max_combined_prio_level + 1 - min_combined_prio) / (max_combined_prio_level + 1)
    │   ├─→ gbr_weight = GBR_DL / avg_rate (GBR QoS Flow인 경우)
    │   ├─→ delay_weight = hol_delay_ms / PDB (HOL delay가 있는 경우)
    │   └─→ pf_weight = compute_pf_metric(estim_dl_rate, avg_dl_rate)
    ↓ combine_qos_metrics():
    │   └─→ final_priority = gbr_weight × pf_weight × prio_weight × delay_weight
    ↓ 스케줄링 결정
```

---

## 수정/추가된 파일 상세

### Open5GS SMF 측

#### 1. 새로 추가된 파일

##### `src/smf/qos-modify.h`
- **목적**: QoS 수정 API 핸들러의 헤더 파일
- **내용**: 
  - `smf_qos_modify_handle_request()` 함수 선언
  - SBI 스트림과 메시지를 받아 QoS 수정 요청을 처리

##### `src/smf/qos-modify.c`
- **목적**: QoS 수정 API의 실제 구현
- **주요 기능**:
  1. **JSON 요청 파싱**: cJSON 라이브러리를 사용하여 요청 본문 파싱
     - 필수: `supi`, `psi`, `qfi`, `5qi`
     - 선택: `gbr_dl`, `gbr_ul`, `mbr_dl`, `mbr_ul`
  2. **UE/세션/QoS Flow 찾기**:
     - `smf_ue_find_by_supi()`: SUPI로 UE 찾기
     - 세션 리스트에서 PSI로 세션 찾기
     - `smf_qos_flow_find_by_qfi()`: QFI로 QoS Flow 찾기
  3. **QoS 파라미터 업데이트**:
     - 5QI 값 변경
     - GBR 값 변경 (제공된 경우)
     - MBR 값 변경 (제공된 경우)
     - **중요**: GBR가 제공되고 MBR이 없는 경우, GBR 값을 MBR로 자동 설정
     - `smf_bearer_qos_update()`: QoS 파라미터 업데이트
  4. **NGAP/PFCP 메시지 전송**:
     - `smf_5gc_pfcp_send_one_qos_flow_modification_request()`: UPF에 PFCP 수정 요청
       - 플래그: `OGS_PFCP_MODIFY_SESSION | OGS_PFCP_MODIFY_NETWORK_REQUESTED | OGS_PFCP_MODIFY_QOS_MODIFY`
     - `gsm_build_pdu_session_modification_command()`: NAS 메시지 생성
     - `ngap_build_pdu_session_resource_modify_request_transfer()`: NGAP 메시지 생성
     - `smf_namf_comm_send_n1_n2_message_transfer()`: AMF로 N1/N2 메시지 전송

**핵심 코드**:
```c
// JSON 파싱
json = cJSON_Parse((char *)message->http.content);
supi = cJSON_GetStringValue(cJSON_GetObjectItem(json, "supi"));
psi = cJSON_GetObjectItem(json, "psi")->valueint;
qfi = cJSON_GetObjectItem(json, "qfi")->valueint;
five_qi = cJSON_GetObjectItem(json, "5qi")->valueint;

// GBR/MBR 파싱 (선택적)
item = cJSON_GetObjectItem(json, "gbr_dl");
if (item && cJSON_IsNumber(item)) {
    gbr_dl = (uint64_t)item->valuedouble;
    has_gbr = true;
}

// QoS 파라미터 업데이트
qos_flow->qos.index = five_qi;
if (has_gbr) {
    if (gbr_dl > 0)
        qos_flow->qos.gbr.downlink = gbr_dl;
    if (gbr_ul > 0)
        qos_flow->qos.gbr.uplink = gbr_ul;
    // MBR 자동 설정
    if (!has_mbr) {
        if (gbr_dl > 0 && qos_flow->qos.mbr.downlink == 0)
            qos_flow->qos.mbr.downlink = gbr_dl;
        if (gbr_ul > 0 && qos_flow->qos.mbr.uplink == 0)
            qos_flow->qos.mbr.uplink = gbr_ul;
    }
}
if (has_mbr) {
    if (mbr_dl > 0)
        qos_flow->qos.mbr.downlink = mbr_dl;
    if (mbr_ul > 0)
        qos_flow->qos.mbr.uplink = mbr_ul;
}

// PFCP 및 NGAP 메시지 전송
smf_5gc_pfcp_send_one_qos_flow_modification_request(
    qos_flow, NULL,
    OGS_PFCP_MODIFY_SESSION |
        OGS_PFCP_MODIFY_NETWORK_REQUESTED |
        OGS_PFCP_MODIFY_QOS_MODIFY,
    0);
smf_namf_comm_send_n1_n2_message_transfer(...);
```

##### `misc/iperf3_dynamic_5qi_test.sh`
- **목적**: iperf3를 사용하여 동적 5QI/GBR/MBR 변경 테스트를 자동화하는 스크립트
- **주요 기능**:
  1. **HTTP/2 클라이언트 확인**: Open5GS SMF는 HTTP/2를 사용하므로 httpx, nghttp2, 또는 curl --http2 필요
  2. **iperf3 서버 시작**: UE 네임스페이스와 외부 서버에서 iperf3 서버 시작
  3. **트래픽 생성**: DL(Downlink)과 UL(Uplink) 트래픽을 동시에 생성
  4. **5QI/GBR/MBR 동적 변경**: 
     - Phase 1: 모든 UE 기본 5QI로 실행 (20초)
     - Phase 2: UE0만 중단 (10초)
     - Phase 3: UE0만 5QI=9 (non-GBR), MBR=5Mbps로 변경하여 실행 (20초)
     - Phase 4: UE0만 중단 (10초)
     - Phase 5: UE0만 5QI=85 (delay_critical GBR), GBR=25Mbps로 변경하여 실행 (20초)

#### 2. 수정된 파일

##### `src/smf/smf-sm.c`
- **수정 내용**: SBI 서버 요청 라우팅에 `/qos-modify` 엔드포인트 추가
- **위치**: `OGS_SBI_SERVICE_NAME_NSMF_PDUSESSION` 케이스 내부
- **추가된 코드**:
```c
CASE("qos-modify")
    SWITCH(sbi_message.h.method)
    CASE(OGS_SBI_HTTP_METHOD_POST)
        smf_qos_modify_handle_request(stream, &sbi_message, request);
        break;
    DEFAULT
        ogs_error("Invalid HTTP method [%s]", sbi_message.h.method);
        ogs_assert(true ==
            ogs_sbi_server_send_error(stream,
                OGS_SBI_HTTP_STATUS_BAD_REQUEST, &sbi_message,
                "Invalid HTTP method", sbi_message.h.method,
                NULL));
    END
    break;
```
- **헤더 추가**:
```c
#include "qos-modify.h"
```

##### `src/smf/meson.build`
- **수정 내용**: 빌드 시스템에 새 파일 추가
- **추가된 항목**:
  - 헤더 파일: `qos-modify.h`
  - 소스 파일: `qos-modify.c`

##### `lib/sbi/message.c`
- **수정 내용**: 커스텀 `qos-modify` 리소스 이름 허용
- **위치**: `OGS_SBI_SERVICE_NAME_NSMF_PDUSESSION` 케이스 내부
- **추가된 코드**:
```c
CASE("qos-modify")
    /*
     * Custom SMF QoS modify API.
     * 표준 OpenAPI 타입으로 파싱할 필요가 없으므로
     * 여기서는 아무 것도 하지 않고 성공으로 둔다.
     * 실제 JSON 파싱은 smf_qos_modify_handle_request()에서 수행.
     */
    break;
```
- **이유**: SBI 메시지 파서가 `qos-modify`를 알 수 없는 리소스로 인식하여 에러를 발생시키는 것을 방지

##### `src/smf/ngap-build.c`
- **수정 내용**: GBR QoS Information 포함 로직 개선
- **위치**: `fill_qos_level_parameters()` 함수
- **주요 변경사항**:
  1. **GBR 정보 포함 조건 개선**: 
     ```c
     // 기존: if (include_gbr && qos->gbr.downlink && qos->gbr.uplink)
     // 변경: 한 방향만 있어도 GBR 정보 포함
     if (include_gbr && (qos->gbr.downlink > 0 || qos->gbr.uplink > 0)) {
     ```
  2. **MBR 값 자동 설정**: MBR이 제공되지 않은 경우 GBR 값을 MBR로 사용
     ```c
     uint64_t gbr_dl = qos->gbr.downlink > 0 ? qos->gbr.downlink : 0;
     uint64_t gbr_ul = qos->gbr.uplink > 0 ? qos->gbr.uplink : 0;
     uint64_t mbr_dl = qos->mbr.downlink > 0 ? qos->mbr.downlink : gbr_dl;
     uint64_t mbr_ul = qos->mbr.uplink > 0 ? qos->mbr.uplink : gbr_ul;
     ```
  3. **디버그 로그 추가**: GBR QoS Information이 포함될 때 상세 정보 로깅
     ```c
     ogs_info("[NGAP-BUILD] GBR QoS Information included: GBR_DL=%llu bps, GBR_UL=%llu bps, "
              "MBR_DL=%llu bps, MBR_UL=%llu bps",
              (unsigned long long)gbr_dl, (unsigned long long)gbr_ul,
              (unsigned long long)mbr_dl, (unsigned long long)mbr_ul);
     ```
- **이유**: GBR QoS Flow에서 MBR이 명시적으로 제공되지 않아도 GBR 값을 MBR로 사용하여 NGAP 메시지에 올바르게 포함되도록 함

---

### srsRAN gNB 측

#### 1. NGAP 레이어: GBR 정보 파싱

##### `lib/ngap/ngap_asn1_helpers.h`
- **수정 내용**: `PDU Session Resource Modify Request`에서 GBR QoS Information 파싱 추가
- **주요 코드**:
```cpp
// PDU Session Resource Modify Request Transfer 파싱 시
if (qos_flow_item->qos_flow_level_qos_params.qos_characteristics.type() ==
    qos_characteristics_s::types_opts::non_dyn_5qi) {
    // ... 기존 코드 ...
    
    // GBR QoS Information 파싱 (추가됨)
    if (qos_flow_item->gbr_qos_flow_info_present) {
        const auto& gbr_info = qos_flow_item->gbr_qos_flow_info;
        // GBR_DL, GBR_UL, MBR_DL, MBR_UL 추출
        // qos_flow_setup_request_item에 GBR 정보 설정
        qos_flow_item_out.gbr_qos_info = gbr_qos_info_from_asn1(gbr_info);
    }
}
```
- **이유**: Open5GS SMF가 NGAP 메시지에 포함한 GBR 정보를 srsRAN gNB가 파싱할 수 있도록 함

---

#### 2. CU-CP: PDU Session Resource Modification Routine

##### `lib/cu_cp/routines/pdu_session_resource_modification_routine.cpp`
- **수정 내용**: F1AP 메시지 생성 시 QoS 정보 포함
- **주요 코드**:
```cpp
// E1AP 응답 처리 후 F1AP 메시지 생성
for (const auto& e1ap_drb_modified_item : e1ap_response.drb_setup_mod_list) {
    drb_id_t drb_id = uint_to_drb_id(e1ap_drb_modified_item.drb_id);
    
    // 기존 DRB 정보 가져오기
    const auto& drb_to_modify = /* ... */;
    
    // F1AP DRB modify item 생성
    f1ap_drb_to_modify drb_modify_item;
    drb_modify_item.drb_id = drb_id;
    
    // UL UP TNL 정보 설정
    for (const auto& ul_up_param : e1ap_drb_modified_item.ul_up_transport_params) {
        drb_modify_item.uluptnl_info_list.push_back(ul_up_param.up_tnl_info);
    }
    
    // QoS 정보 포함 (추가됨)
    f1ap_drb_info qos_info;
    qos_info.drb_qos = drb_to_modify.qos_params;
    qos_info.s_nssai = drb_to_modify.s_nssai;
    
    // QoS flows 정보 포함
    for (const auto& flow : drb_to_modify.qos_flows) {
        flow_mapped_to_drb flow_item;
        flow_item.qos_flow_id = flow.first;
        flow_item.qos_flow_level_qos_params = flow.second.qos_params;
        qos_info.flows_mapped_to_drb_list.push_back(flow_item);
    }
    
    drb_modify_item.qos_info = qos_info;  // 추가됨
    
    // 로깅
    logger.debug("Created f1ap_drb_to_modify for DRB {}: 5QI={}, ARP_priority={}, NoF_QoS_Flows={}",
                 drb_modify_item.drb_id,
                 drb_modify_item.qos_info.value().drb_qos.qos_desc.get_5qi(),
                 drb_modify_item.qos_info.value().drb_qos.alloc_retention_prio.prio_level_arp.value(),
                 drb_modify_item.qos_info.value().flows_mapped_to_drb_list.size());
    
    ue_context_mod_request.drbs_to_be_modified_list.push_back(drb_modify_item);
}
```
- **이유**: CU-CP에서 DU로 DRB 수정 요청을 보낼 때 QoS 정보를 포함하여 DU가 QoS 파라미터를 업데이트할 수 있도록 함

---

#### 3. F1AP: QoS 정보 구조체 추가

##### `include/srsran/f1ap/ue_context_management_configs.h`
- **수정 내용**: `f1ap_drb_to_modify` 구조체에 `qos_info` 필드 추가
- **주요 코드**:
```cpp
struct f1ap_drb_to_modify {
    /// DRB-Id of the existing DRB.
    drb_id_t drb_id = drb_id_t::invalid;
    /// UL Transport layer info for the given DRB.
    std::vector<up_transport_layer_info> uluptnl_info_list;
    /// QoS Information of the DRB.  // 추가됨
    std::optional<f1ap_drb_info> qos_info;  // 추가됨
};
```
- **이유**: F1AP 프로토콜을 통해 CU-CP에서 DU로 QoS 정보를 전달하기 위한 구조체 확장

---

#### 4. F1AP: ASN.1 인코딩/디코딩

##### `lib/f1ap/asn1_helpers.cpp`
- **수정 내용**: `drbs_to_be_modified_item_s` ASN.1 메시지에 `qos_info` 인코딩/디코딩 추가
- **주요 코드**:
```cpp
// 인코딩 (CU-CP → DU)
drbs_to_be_modified_item_s make_drb_to_mod(const f1ap_drb_to_modify& drb_item)
{
    drbs_to_be_modified_item_s asn1type;
    asn1type.drb_id = drb_id_to_uint(drb_item.drb_id);
    asn1type.ul_up_tnl_info_to_be_setup_list = make_asn1_ul_up_tnl_info_list(drb_item.uluptnl_info_list);
    
    // QoS info 인코딩 (추가됨)
    if (drb_item.qos_info.has_value()) {
        asn1type.qos_info_present = true;
        asn1type.qos_info = qos_info_to_f1ap_asn1(drb_item.qos_info.value());
    }
    
    return asn1type;
}

// 디코딩 (DU ← CU-CP)
f1ap_drb_to_modify make_drb_to_modify(const drbs_to_be_modified_item_s& drb_item)
{
    f1ap_drb_to_modify drb_obj;
    drb_obj.drb_id = uint_to_drb_id(drb_item.drb_id);
    drb_obj.uluptnl_info_list = make_ul_up_tnl_info_list(drb_item.ul_up_tnl_info_to_be_setup_list);
    
    // QoS info 디코딩 (추가됨)
    if (drb_item.qos_info_present) {
        drb_obj.qos_info = drb_info_from_f1ap_asn1(drb_item.qos_info);
    }
    
    return drb_obj;
}
```
- **이유**: F1AP ASN.1 메시지에 QoS 정보를 포함하여 전송하고 수신할 수 있도록 함

---

#### 5. DU: DRB 수정 시 QoS 업데이트

##### `lib/du/du_high/du_manager/ran_resource_management/du_bearer_resource_manager.cpp`
- **수정 내용**: `modify_drbs()` 함수에서 QoS 정보 업데이트
- **주요 코드**:
```cpp
std::vector<drb_id_t> du_bearer_resource_manager::modify_drbs(
    du_ue_resource_config& ue_cfg,
    const du_ue_bearer_resource_update_request& upd_req)
{
    std::vector<drb_id_t> failed_drbs;
    
    for (const f1ap_drb_to_modify& drb_to_modify : upd_req.drbs_to_mod) {
        // 검증
        auto res = validate_drb_modification_request(drb_to_modify, ue_cfg.drbs);
        if (not res.has_value()) {
            logger.warning("Failed to modify {}. Cause: {}", drb_to_modify.drb_id, res.error());
            failed_drbs.push_back(drb_to_modify.drb_id);
            continue;
        }
        
        logger.debug("Processing DRB modification request for DRB {}", drb_to_modify.drb_id);
        
        // QoS 정보 업데이트 (추가됨)
        if (drb_to_modify.qos_info.has_value()) {
            auto& existing_drb = ue_cfg.drbs[drb_to_modify.drb_id];
            
            // 이전 5QI 저장
            five_qi_t old_5qi = existing_drb.qos.qos_desc.get_5qi();
            
            // DRB QoS 파라미터 업데이트
            existing_drb.qos = drb_to_modify.qos_info.value().drb_qos;
            existing_drb.s_nssai = drb_to_modify.qos_info.value().s_nssai;
            
            // 새 5QI
            five_qi_t new_5qi = existing_drb.qos.qos_desc.get_5qi();
            
            // QoS characteristics 가져오기
            const auto* qos_chars = get_5qi_to_qos_characteristics_mapping(new_5qi);
            if (qos_chars != nullptr) {
                // GBR 정보 추출
                uint64_t gbr_dl = 0, gbr_ul = 0, mbr_dl = 0, mbr_ul = 0;
                if (existing_drb.qos.gbr_qos_info.has_value()) {
                    gbr_dl = existing_drb.qos.gbr_qos_info.value().gbr_dl;
                    gbr_ul = existing_drb.qos.gbr_qos_info.value().gbr_ul;
                    mbr_dl = existing_drb.qos.gbr_qos_info.value().max_br_dl;
                    mbr_ul = existing_drb.qos.gbr_qos_info.value().max_br_ul;
                }
                
                // 로깅
                logger.info("Updated QoS for DRB {}: 5QI={}->{}, priority_level={}, ARP_priority={}, "
                           "GBR_DL={} bps, GBR_UL={} bps, MBR_DL={} bps, MBR_UL={} bps",
                           drb_to_modify.drb_id,
                           old_5qi, new_5qi,
                           qos_chars->priority.value(),
                           existing_drb.qos.alloc_retention_prio.prio_level_arp.value(),
                           gbr_dl, gbr_ul, mbr_dl, mbr_ul);
            } else {
                logger.warning("Updated QoS for DRB {}: 5QI={}->{}, but no QoS characteristics mapping found",
                               drb_to_modify.drb_id, old_5qi, new_5qi);
            }
        } else {
            logger.warning("DRB {} modification request does not include QoS info - QoS will not be updated",
                           drb_to_modify.drb_id);
        }
    }
    
    return failed_drbs;
}
```
- **이유**: F1AP를 통해 받은 QoS 정보를 DU의 UE 리소스 설정에 반영하여 스케줄러가 올바른 QoS 파라미터를 사용하도록 함

---

#### 6. CU-CP: UP Resource Manager - DRB 수정 처리

##### `lib/cu_cp/up_resource_manager/up_resource_manager_impl.cpp`
- **수정 내용**: `apply_config_update()` 함수에 `drb_to_modify` 처리 추가
- **주요 코드**:
```cpp
// DRB 수정을 위한 헬퍼 함수 (추가됨)
static void apply_update_for_modified_drbs(
    up_pdu_session_context& pdu_session_context,
    up_context& context,
    const std::map<drb_id_t, up_drb_context>& drb_to_modify)
{
    for (const auto& drb : drb_to_modify) {
        // DRB 존재 확인
        if (pdu_session_context.drbs.find(drb.first) == pdu_session_context.drbs.end()) {
            // DRB가 존재하지 않음 - modify에서는 발생하지 않아야 하지만 안전하게 처리
            continue;
        }
        
        auto& existing_drb = pdu_session_context.drbs.at(drb.first);
        
        // QoS 파라미터 업데이트
        existing_drb.qos_params = drb.second.qos_params;
        
        // QoS flows 업데이트
        for (const auto& flow : drb.second.qos_flows) {
            // Update or add QoS flow
            existing_drb.qos_flows[flow.first] = flow.second;
            // Update QoS flow map
            context.qos_flow_map[flow.first] = drb.first;
        }
        
        // 기타 DRB 파라미터 업데이트
        existing_drb.pdcp_cfg = drb.second.pdcp_cfg;
        existing_drb.sdap_cfg = drb.second.sdap_cfg;
        existing_drb.rlc_mod = drb.second.rlc_mod;
        
        // DRB Id를 dirty로 표시 (키 갱신 필요)
        context.drb_dirty[get_dirty_drb_index(drb.first)] = true;
    }
}

bool up_resource_manager::apply_config_update(const up_config_update_result& result)
{
    // ... 기존 코드 (새 세션 추가) ...
    
    for (const auto& mod_session : result.pdu_sessions_modified_list) {
        srsran_assert(
            context.pdu_sessions.find(mod_session.id) != context.pdu_sessions.end(), 
            "{} not allocated", mod_session.id);
        auto& session_context = context.pdu_sessions.at(mod_session.id);
        
        // DRB 수정 처리 (추가됨)
        apply_update_for_modified_drbs(session_context, context, mod_session.drb_to_modify);
        
        // ... 기존 코드 (새 DRB 추가, DRB 제거) ...
    }
    
    // ... 기존 코드 (세션 제거) ...
    
    return true;
}
```
- **이유**: UP Resource Manager가 DRB 수정 요청을 처리하여 UP 컨텍스트에 QoS 변경사항을 반영

---

#### 7. CU-CP: UP Resource Manager Helper - modify_qos_flow 수정

##### `lib/cu_cp/up_resource_manager/up_resource_manager_helpers.cpp`
- **수정 내용**: `modify_qos_flow()` 함수에서 기존 DRB 정보 보존
- **주요 코드**:
```cpp
static drb_id_t modify_qos_flow(
    up_pdu_session_context_update& new_session_context,
    const qos_flow_setup_request_item& qos_flow,
    const up_config_update& config_update,
    const up_context& full_context,
    const up_resource_manager_cfg& cfg,
    const srslog::basic_logger& logger)
{
    // ... 기존 코드 (QoS flow 찾기, DRB ID 확인) ...
    
    drb_id_t drb_id = full_context.qos_flow_map.at(qos_flow.qos_flow_id);
    
    // 기존 DRB 컨텍스트 가져오기 (추가됨)
    const auto& existing_drb = full_context.pdu_sessions.at(new_session_context.id)
                                  .drbs.at(drb_id);
    
    // 기존 DRB 정보로 시작 (추가됨)
    up_drb_context drb_ctx = existing_drb;
    
    // QoS 파라미터만 업데이트
    drb_ctx.qos_params = qos_flow.qos_flow_level_qos_params;
    
    // 수정된 QoS flow 업데이트
    up_qos_flow_context flow_ctx;
    flow_ctx.qfi = qos_flow.qos_flow_id;
    flow_ctx.qos_params = qos_flow.qos_flow_level_qos_params;
    drb_ctx.qos_flows[flow_ctx.qfi] = flow_ctx;
    
    // PDCP/SDAP 설정 업데이트 (새 QoS에 맞게)
    drb_ctx.pdcp_cfg = set_rrc_pdcp_config(qos_flow.qos_flow_level_qos_params.qos_desc.get_5qi(), cfg);
    drb_ctx.sdap_cfg = set_rrc_sdap_config(drb_ctx);
    drb_ctx.rlc_mod = (drb_ctx.pdcp_cfg.rlc_mode == pdcp_rlc_mode::am) ? rlc_mode::am : rlc_mode::um_bidir;
    
    // 수정된 DRB 추가
    new_session_context.drb_to_modify.emplace(drb_id, drb_ctx);
    
    return drb_id;
}
```
- **이유**: QoS 수정 시 기존 DRB의 모든 정보(예: RLC 상태, PDCP 시퀀스 번호 등)를 보존하고 QoS 파라미터만 업데이트

---

#### 8. DU: 스케줄러 설정 헬퍼

##### `lib/du/du_high/du_manager/converters/scheduler_configuration_helpers.cpp`
- **수정 내용**: DRB QoS 정보를 스케줄러에 전달 (기존 로직 확인 및 로깅 추가)
- **주요 코드**:
```cpp
// Logical Channel QoS Config 설정 시
if (drb.qos.gbr_qos_info.has_value()) {
    // GBR QoS Flow
    lc_cfg.qos->gbr_qos_info = drb.qos.gbr_qos_info.value();
    logger.info("[SCHED-LC] Logical Channel QoS Config: ue={}, DRB={}, LCID={}, "
               "5QI={}, has_gbr=true, GBR_DL={} bps, GBR_UL={} bps",
               ue_idx, drb_id, lcid, 5qi,
               lc_cfg.qos->gbr_qos_info.value().gbr_dl,
               lc_cfg.qos->gbr_qos_info.value().gbr_ul);
} else {
    // non-GBR QoS Flow
    logger.info("[SCHED-LC] Logical Channel QoS Config: ue={}, DRB={}, LCID={}, "
               "5QI={}, has_gbr=false (non-GBR flow)",
               ue_idx, drb_id, lcid, 5qi);
}
```
- **이유**: DU에서 DRB QoS 정보를 스케줄러의 Logical Channel 설정에 반영

---

#### 9. 스케줄러: QoS 가중치 계산

##### `lib/scheduler/policy/scheduler_time_qos.cpp`
- **수정 내용**: GBR weight 및 delay_weight 계산 로직 및 로깅
- **주요 코드**:
```cpp
static double compute_dl_qos_weights(
    const slice_ue& u,
    double estim_dl_rate,
    double avg_dl_rate,
    slot_point slot_tx,
    const time_qos_scheduler_config& policy_params)
{
    if (avg_dl_rate == 0) {
        // Highest priority to UEs that have not yet received any allocation.
        return std::numeric_limits<double>::max();
    }
    
    static auto& logger = srslog::fetch_basic_logger("SCHED", false);
    static constexpr uint16_t max_combined_prio_level = qos_prio_level_t::max() * arp_prio_level_t::max();
    uint16_t min_combined_prio = max_combined_prio_level;
    double gbr_weight = 0;
    double delay_weight = 0;
    
    if (policy_params.gbr_enabled or policy_params.priority_enabled or policy_params.pdb_enabled) {
        for (logical_channel_config_ptr lc : *u.logical_channels()) {
            if (not u.contains(lc->lcid) or not lc->qos.has_value() or 
                u.pending_dl_newtx_bytes(lc->lcid) == 0) {
                continue;
            }
            
            // Priority weight 계산
            if (policy_params.priority_enabled) {
                min_combined_prio = std::min(
                    static_cast<uint16_t>(lc->qos->qos.priority.value() * lc->qos->arp_priority.value()), 
                    min_combined_prio);
            }
            
            // Delay weight 계산 (HOL delay 기반)
            slot_point hol_toa = u.dl_hol_toa(lc->lcid);
            if (hol_toa.valid() and slot_tx >= hol_toa) {
                const unsigned hol_delay_ms = 
                    (slot_tx - hol_toa) / slot_tx.nof_slots_per_subframe();
                const unsigned pdb = lc->qos->qos.packet_delay_budget_ms;
                delay_weight += hol_delay_ms / static_cast<double>(pdb);
            }
            
            // GBR weight 계산
            if (not lc->qos->gbr_qos_info.has_value()) {
                // LC is a non-GBR flow.
                continue;
            }
            
            // GBR flow.
            double dl_avg_rate = u.dl_avg_bit_rate(lc->lcid);
            if (dl_avg_rate != 0) {
                // NOTE: dl_avg_rate is based on scheduled bytes, not actually transmitted bytes.
                // This means avg_rate can be higher than actual transmission rate due to:
                // - HARQ retransmissions (scheduled but not successfully transmitted)
                // - Channel conditions (scheduled but failed transmission)
                // Therefore, gbr_weight calculation may underestimate GBR guarantee needs.
                double calculated_gbr_weight = 
                    lc->qos->gbr_qos_info->gbr_dl / dl_avg_rate;
                gbr_weight += std::min(calculated_gbr_weight, max_metric_weight);
                
                // 로깅 (100번마다)
                static unsigned gbr_log_counter = 0;
                if ((gbr_log_counter++ % 100) == 0) {
                    logger.info("GBR weight calculation: LCID={}, GBR_DL={} bps, "
                               "avg_rate={} bps, calculated_gbr_weight={:.6f}, "
                               "final_gbr_weight={:.6f}",
                               fmt::underlying(lc->lcid),
                               lc->qos->gbr_qos_info->gbr_dl,
                               dl_avg_rate,
                               calculated_gbr_weight,
                               gbr_weight);
                }
            } else {
                gbr_weight += max_metric_weight;
            }
        }
    }
    
    // If no QoS flows are configured, the weight is set to 1.0.
    gbr_weight = policy_params.gbr_enabled and gbr_weight != 0 ? gbr_weight : 1.0;
    delay_weight = policy_params.pdb_enabled and delay_weight != 0 ? delay_weight : 1.0;
    
    double pf_weight = compute_pf_metric(estim_dl_rate, avg_dl_rate, policy_params.pf_fairness_coeff);
    
    // If priority is disabled, set the priority weight of all UEs to 1.0.
    double prio_weight = policy_params.priority_enabled ? 
        (max_combined_prio_level + 1 - min_combined_prio) /
            static_cast<double>(max_combined_prio_level + 1) : 1.0;
    
    // 로깅 (100번마다)
    static unsigned dl_prio_log_counter = 0;
    if ((dl_prio_log_counter++ % 100) == 0) {
        logger.info("DL Priority calc: UE{} min_combined_prio={}, prio_weight={:.3f}, "
                   "pf_weight={:.3f}, gbr_weight={:.3f}, delay_weight={:.3f}",
                   u.ue_index(), min_combined_prio, prio_weight, pf_weight, 
                   gbr_weight, delay_weight);
    }
    
    // 최종 priority 계산
    return combine_qos_metrics(pf_weight, gbr_weight, prio_weight, delay_weight, policy_params);
}

static double combine_qos_metrics(
    double pf_weight,
    double gbr_weight,
    double prio_weight,
    double delay_weight,
    const time_qos_scheduler_config& policy_params)
{
    if (policy_params.combine_function == 
        time_qos_scheduler_config::combine_function_type::gbr_prioritized and
        gbr_weight > 1.0) {
        // GBR target has not been met and we prioritize GBR over PF.
        pf_weight = std::max(1.0, pf_weight);
    }
    
    // 최종 가중치: gbr_weight × pf_weight × prio_weight × delay_weight
    return gbr_weight * pf_weight * prio_weight * delay_weight;
}
```
- **이유**: 
  - GBR weight: `GBR_DL / avg_rate`로 계산하여 GBR이 충족되지 않으면 우선순위 증가
  - Delay weight: HOL delay를 PDB로 나눈 값으로 계산하여 지연이 큰 패킷에 우선순위 부여
  - 최종 priority: 모든 가중치를 곱하여 계산

---

#### 10. RLC → MAC → Scheduler: HOL TOA 전달 경로

##### `include/srsran/rlc/rlc_buffer_state.h`
- **수정 내용**: `hol_toa` 타입을 `system_clock`으로 변경
- **주요 코드**:
```cpp
struct rlc_buffer_state {
    /// Amount of bytes pending for transmission.
    unsigned pending_bytes = 0;
    /// Head of line (HOL) time of arrival (TOA) holds the TOA of the oldest SDU or ReTx that is queued for transmission.
    // steady_clock에서 system_clock로 변경 (추가됨)
    std::optional<std::chrono::system_clock::time_point> hol_toa;
};
```

##### `lib/rlc/rlc_tx_am_entity.cpp`
- **수정 내용**: `steady_clock`을 `system_clock`으로 변환하는 헬퍼 함수 추가 및 적용
- **주요 코드**:
```cpp
namespace {
// steady_clock을 system_clock으로 변환하는 헬퍼 함수 (추가됨)
std::optional<std::chrono::system_clock::time_point>
convert_steady_to_system(const std::optional<std::chrono::steady_clock::time_point>& steady_tp)
{
    if (not steady_tp.has_value()) {
        return std::nullopt;
    }
    auto now_steady = std::chrono::steady_clock::now();
    auto delay = now_steady - steady_tp.value();
    return std::chrono::system_clock::now() - 
           std::chrono::duration_cast<std::chrono::system_clock::duration>(delay);
}
}

rlc_buffer_state rlc_tx_am_entity::get_buffer_state()
{
    rlc_buffer_state bs = {};
    // ... 기존 코드 ...
    
    // 변환 적용 (추가됨)
    if (sn_under_segmentation != INVALID_RLC_SN) {
        if (tx_window.has_sn(sn_under_segmentation)) {
            rlc_tx_am_sdu_info& sdu_info = tx_window[sn_under_segmentation];
            bs.hol_toa = convert_steady_to_system(sdu_info.time_of_arrival);
        }
    } else {
        const rlc_sdu* next_sdu = sdu_queue.front();
        if (next_sdu != nullptr) {
            bs.hol_toa = convert_steady_to_system(next_sdu->time_of_arrival);
        }
    }
    
    // ... 기존 코드 ...
    
    if (!retx_queue.empty()) {
        bs.hol_toa = convert_steady_to_system(tx_window[retx_queue.front().sn].time_of_arrival);
    }
    
    return bs;
}
```

##### `lib/rlc/rlc_tx_um_entity.cpp`
- **수정 내용**: 동일한 변환 로직 추가

##### `lib/rlc/rlc_tx_tm_entity.cpp`
- **수정 내용**: 동일한 변환 로직 추가

##### `lib/mac/mac_dl/mac_cell_processor.cpp`
- **수정 내용**: RLC buffer state에서 `hol_toa`를 MAC 메시지로 전달
- **주요 코드**:
```cpp
// handle_dl_data_indication() 함수 내
rlc_buffer_state rlc_bs = bearer->on_buffer_state_update();
mac_dl_buffer_state_indication_message bs{
    ue_mng.get_ue_index(grant.pdsch_cfg.rnti), 
    lc_info.lcid.to_lcid(), 
    rlc_bs.pending_bytes};

// hol_toa 전달 (추가됨)
if (rlc_bs.hol_toa.has_value()) {
    bs.hol_toa = rlc_bs.hol_toa.value();
}
sched.handle_dl_buffer_state_update(bs);
```
- **이유**: 
  - RLC에서 계산한 HOL TOA를 Scheduler까지 전달하여 delay_weight 계산에 사용
  - `steady_clock`은 상대 시간이므로 `system_clock`으로 변환하여 상위 레이어에서 사용 가능하도록 함

**참고**: MAC → Scheduler 경로는 이미 구현되어 있었으며, `srsran_scheduler_adapter.cpp`에서 `system_clock::time_point`를 `slot_point`로 변환하는 로직이 존재함

---

#### 11. 스케줄러 메트릭: Throughput 로깅

##### `lib/scheduler/logging/scheduler_metrics_handler.cpp`
- **수정 내용**: UE별 throughput 계산 및 로깅 추가
- **주요 코드**:
```cpp
scheduler_ue_metrics
cell_metrics_handler::ue_metric_context::compute_report(
    std::chrono::milliseconds metric_report_period,
    unsigned slots_per_sf)
{
    // ... 기존 코드 ...
    
    ret.dl_brate_kbps = static_cast<double>(data.sum_dl_tb_bytes * 8U) / 
                        metric_report_period.count();
    ret.ul_brate_kbps = static_cast<double>(data.sum_ul_tb_bytes * 8U) / 
                        metric_report_period.count();
    ret.dl_nof_ok = data.count_uci_harq_acks;
    ret.ul_nof_ok = data.count_crc_acks;
    
    // Throughput 로깅 (추가됨)
    static auto& logger = srslog::fetch_basic_logger("SCHED", false);
    double dl_brate_mbps = ret.dl_brate_kbps / 1000.0;
    double ul_brate_mbps = ret.ul_brate_kbps / 1000.0;
    
    if (ret.ul_brate_kbps > 0) {
        logger.info("UE{} Throughput calc: sum_dl_tb_bytes={}, period={}ms, "
                   "dl_brate_kbps={:.2f} (={:.2f}Mbps), dl_nof_ok={}, "
                   "ul_brate_kbps={:.2f} (={:.2f}Mbps), ul_nof_ok={}",
                   ue_index, data.sum_dl_tb_bytes, metric_report_period.count(),
                   ret.dl_brate_kbps, dl_brate_mbps, ret.dl_nof_ok,
                   ret.ul_brate_kbps, ul_brate_mbps, ret.ul_nof_ok);
    } else {
        logger.info("UE{} Throughput calc: sum_dl_tb_bytes={}, period={}ms, "
                   "dl_brate_kbps={:.2f} (={:.2f}Mbps), dl_nof_ok={}",
                   ue_index, data.sum_dl_tb_bytes, metric_report_period.count(),
                   ret.dl_brate_kbps, dl_brate_mbps, ret.dl_nof_ok);
    }
    
    return ret;
}
```
- **이유**: Python 스크립트로 throughput 정보를 추출할 수 있도록 로그 형식 제공

---

### Python 분석 스크립트

#### 1. `misc/extract_ue_priority.py`
- **목적**: UE별 Priority 정보 추출 (min_combined_prio, prio_weight, pf_weight, gbr_weight, delay_weight)
- **로그 패턴**: `DL Priority calc: UE{} min_combined_prio={}, prio_weight={:.3f}, ...`
- **출력**: 1초 윈도우별 집계 및 상세 정보
- **사용법**: `python extract_ue_priority.py [log_file] [-u ue_idx] [-c csv_file]`

#### 2. `misc/extract_ue_throughput.py`
- **목적**: UE별 Throughput 정보 추출 (DL/UL bitrate)
- **로그 패턴**: `UE{} Throughput calc: sum_dl_tb_bytes={}, period={}ms, dl_brate_kbps={:.2f} ...`
- **출력**: 시스템 계산값 기반 throughput 통계
- **사용법**: `python extract_ue_throughput.py [log_file] [-u ue_idx] [-c csv_file]`

#### 3. `misc/extract_ue_hol_delay.py`
- **목적**: UE별 HOL Delay (큐잉 딜레이) 정보 추출
- **로그 패턴**: `[DELAY-WEIGHT] UE{} LCID{} hol_toa={} slot_tx={} hol_delay_ms={} ...`
- **출력**: 시스템 시간과 hol_delay_ms만 표시
- **사용법**: `python extract_ue_hol_delay.py [log_file] [-u ue_idx] [-c csv_file]`

---

## 최종 구현 상태

### Open5GS SMF 측
- ✅ 커스텀 API `/nsmf-pdusession/v1/qos-modify` 구현
- ✅ JSON 요청 파싱 및 QoS 파라미터 업데이트
- ✅ PFCP 및 NGAP 메시지 전송
- ✅ GBR QoS Information을 NGAP 메시지에 포함
- ✅ MBR 자동 설정 (GBR이 제공되고 MBR이 없는 경우)

### srsRAN gNB 측
- ✅ NGAP에서 GBR 정보 파싱
- ✅ CU-CP에서 F1AP로 QoS 정보 전달
- ✅ DU에서 DRB 수정 시 QoS 업데이트
- ✅ 스케줄러에 GBR 정보 전달
- ✅ GBR weight 및 delay_weight 계산
- ✅ RLC → MAC → Scheduler HOL TOA 전달 경로 완성

### 스케줄러 동작
- ✅ `gbr_weight = GBR_DL / avg_rate` 계산
- ✅ `delay_weight = hol_delay_ms / PDB` 계산
- ✅ `final_priority = gbr_weight × pf_weight × prio_weight × delay_weight`
- ✅ GBR이 충족되지 않으면 우선순위 증가
- ✅ HOL delay가 클수록 우선순위 증가

### 분석 도구
- ✅ Priority 정보 추출 스크립트
- ✅ Throughput 정보 추출 스크립트
- ✅ HOL Delay 정보 추출 스크립트

---

## 빌드 및 실행 방법

### 1. Open5GS 빌드

```bash
cd open5gs-1
meson build
ninja -C build
sudo ninja -C build install
```

### 2. srsRAN 빌드

```bash
cd srsRAN_Project-2
mkdir build && cd build
cmake ..
make -j$(nproc)
sudo make install
```

### 3. SMF 재시작

```bash
sudo systemctl restart open5gs-smfd
# 또는
sudo ./build/src/smf/open5gs-smfd
```

### 4. 스크립트 실행

```bash
# 기본 설정으로 실행 (non-GBR: MBR 5Mbps, delay_critical GBR: GBR 25Mbps)
./misc/iperf3_dynamic_5qi_test.sh

# GBR/MBR 값 커스터마이징
MBR_NON_GBR_DL=10000000 MBR_NON_GBR_UL=10000000 \
GBR_DELAY_CRITICAL_DL=50000000 GBR_DELAY_CRITICAL_UL=50000000 \
./misc/iperf3_dynamic_5qi_test.sh
```

---

## 결과 확인

### 1. iperf3 로그 확인
```bash
# Phase별 bitrate 확인
tail -20 /tmp/iperf3_dl0.log          # Phase 1 (기본 5QI)
tail -20 /tmp/iperf3_dl0_phase3.log  # Phase 3 (5QI=9, MBR 5Mbps)
tail -20 /tmp/iperf3_dl0_phase5.log  # Phase 5 (5QI=85, GBR 25Mbps)
```

### 2. SMF 로그 확인
```bash
sudo tail -f /var/log/open5gs/smf.log | grep -E 'Modifying QoS Flow|5QI|QFI|GBR|gbr|MBR|mbr|NGAP-BUILD'
```

### 3. gNB/srsRAN 로그 확인
```bash
tail -f /tmp/gnb.log | grep -iE 'qos|modif|pdu.*session.*resource.*modif|gbr|GBR|mbr|MBR|Updated QoS|Priority calc|GBR weight|Throughput calc|DELAY-WEIGHT'
```

### 4. Python 스크립트로 분석
```bash
# Priority 정보 추출
python misc/extract_ue_priority.py /tmp/gnb.log -u 0 -c ue0_priority.csv

# Throughput 정보 추출
python misc/extract_ue_throughput.py /tmp/gnb.log -u 0 -c ue0_throughput.csv

# HOL Delay 정보 추출
python misc/extract_ue_hol_delay.py /tmp/gnb.log -u 0 -c ue0_hol_delay.csv
```

### 5. API 호출 확인
```bash
# non-GBR 5QI=9, MBR 5Mbps
curl --http2 -X POST http://127.0.0.4:7777/nsmf-pdusession/v1/qos-modify \
  -H 'Content-Type: application/json' \
  -d '{
    "supi": "imsi-001010123456780",
    "psi": 1,
    "qfi": 1,
    "5qi": 9,
    "mbr_dl": 5000000,
    "mbr_ul": 5000000
  }' -v

# delay_critical GBR 5QI=85, GBR 25Mbps
curl --http2 -X POST http://127.0.0.4:7777/nsmf-pdusession/v1/qos-modify \
  -H 'Content-Type: application/json' \
  -d '{
    "supi": "imsi-001010123456780",
    "psi": 1,
    "qfi": 1,
    "5qi": 85,
    "gbr_dl": 25000000,
    "gbr_ul": 25000000
  }' -v
```

---

## 주의사항

1. **HTTP/2 필수**: Open5GS SMF는 HTTP/2를 사용하므로 반드시 HTTP/2 클라이언트가 필요합니다.
   - Python httpx: `pip3 install httpx`
   - nghttp2: `sudo apt-get install nghttp2-client`
   - curl (HTTP/2 지원 버전)

2. **GBR QoS Flow**: 5QI 1-4, 65-67, 82-85는 GBR QoS Flow입니다. 이 경우 GBR 값을 제공해야 합니다.
   - delay_critical GBR (5QI 82-85): GBR 값이 srsRAN 스케줄러의 `dl_gbr`로 사용됩니다.

3. **non-GBR QoS Flow**: 5QI 5-9, 69, 70-79 등은 non-GBR QoS Flow입니다. 
   - MBR 값을 제공하면 srsRAN 스케줄러의 `dl_gbr`로 사용됩니다 (non-GBR이지만 MBR을 GBR처럼 사용).

4. **세션 상태**: PDU Session이 활성화되어 있어야 합니다 (`establishment_accept_sent == true`).

5. **네트워크 네임스페이스**: UE는 네트워크 네임스페이스(`ue1`, `ue2`, `ue3`)에서 실행되어야 합니다.

---

## 문제 해결

### API 호출 실패
- SMF API 주소 확인: `netstat -tlnp | grep 7777`
- HTTP/2 클라이언트 설치 확인
- SMF 로그에서 에러 메시지 확인

### QoS 변경이 적용되지 않음
- gNB 로그에서 NGAP 메시지 수신 확인
- UE 로그에서 NAS 메시지 수신 확인
- PFCP 세션이 활성화되어 있는지 확인
- DU 로그에서 "Updated QoS for DRB" 메시지 확인

### 스케줄러에 QoS 정보가 전달되지 않음
- DU 로그에서 "Logical Channel QoS Config" 메시지 확인
- 스케줄러 로그에서 "GBR weight calculation" 메시지 확인
- `gbr_weight`가 1.0이 아닌지 확인

### delay_weight가 항상 1.0
- RLC 로그에서 HOL TOA 계산 확인
- MAC 로그에서 `hol_toa` 전달 확인
- 스케줄러 로그에서 `hol_toa_valid=true` 확인

---

## 참고 자료

- [3GPP TS 23.501 - 5G System Architecture](https://www.3gpp.org/DynaReport/23501.htm)
- [3GPP TS 29.502 - Nsmf_PDUSession Service API](https://www.3gpp.org/DynaReport/29502.htm)
- [Open5GS Documentation](https://open5gs.org/open5gs/docs/)
- [srsRAN Documentation](https://docs.srsran.com/)

---

## 작성자

이 구현은 Open5GS 커뮤니티를 위해 작성되었습니다.

## 라이선스

Open5GS와 동일한 라이선스 (GNU Affero General Public License v3.0)를 따릅니다.
