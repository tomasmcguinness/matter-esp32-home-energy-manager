#include "matter_controller.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_controller_client.h>
#include <esp_matter_controller_console.h>
#include <esp_matter_controller_credentials_issuer.h>
#include <esp_matter_controller_pairing_command.h>

#include <app/server/Dnssd.h>
#include <controller/CHIPDeviceController.h>
#include <controller/DevicePairingDelegate.h>
#include <controller/OperationalCredentialsDelegate.h>
#include <credentials/CHIPCert.h>
#include <crypto/CHIPCryptoPAL.h>
#include <lib/core/CASEAuthTag.h>
#include <lib/core/TLV.h>
#include <lib/support/CHIPMem.h>
#include <lib/support/Span.h>
#include <platform/CHIPDeviceLayer.h>
#include <protocols/secure_channel/PASESession.h>
#include <setup_payload/ManualSetupPayloadParser.h>
#include <setup_payload/QRCodeSetupPayloadParser.h>
#include <setup_payload/SetupPayload.h>

static const char *TAG = "matter_controller";

// static constexpr char     kRootCAKeypairStorageKey[] = "HEM_RootCAKey";
// static constexpr char     kRootCACertStorageKey[]    = "HEM_RootCACert";
// static constexpr char     kIPKStorageKey[]           = "HEM_IPK";
static constexpr char     kNodeIdCounterKey[]        = "HEM_NodeIdCnt";
static constexpr char     kNodeListKey[]             = "HEM_NodeList";
static constexpr size_t   kMaxNodes                  = 32;
// static constexpr uint32_t kCertValiditySeconds       = 10 * 365 * 24 * 3600;
// static constexpr uint64_t kRootCAIssuerId            = 0xAB12AB12AB12AB12ULL;
// static constexpr uint64_t kFabricId                  = 1;
// static constexpr size_t   kIPKLength                 = 16;
static constexpr uint64_t kFirstDeviceNodeId         = 1;

// static uint8_t s_ipk[kIPKLength];

// // ---------------------------------------------------------------------------
// // Credentials issuer — signs both the controller NOC at startup and device
// // NOCs during commissioning via GenerateNOCChain.
// // ---------------------------------------------------------------------------

// class home_energy_manager_credentials_issuer
//     : public esp_matter::controller::credentials_issuer
//     , public chip::Controller::OperationalCredentialsDelegate
// {
// public:
//     esp_err_t initialize_credentials_issuer(chip::PersistentStorageDelegate &storage) override
//     {
//         ESP_LOGI(TAG, "initialize_credentials_issuer");

//         m_storage = &storage;

//         chip::ASN1::ASN1UniversalTime effective_time;
//         CHIP_ZERO_AT(effective_time);
//         effective_time.Year  = 2021;
//         effective_time.Month = 1;
//         effective_time.Day   = 1;
//         if (chip::Credentials::ASN1ToChipEpochTime(effective_time, m_now) != CHIP_NO_ERROR) {
//             ESP_LOGE(TAG, "Failed to convert start time");
//             return ESP_FAIL;
//         }

//         chip::Crypto::P256SerializedKeypair serialized_key;
//         uint16_t key_size = static_cast<uint16_t>(serialized_key.Capacity());
//         CHIP_ERROR err    = storage.SyncGetKeyValue(kRootCAKeypairStorageKey, serialized_key.Bytes(), key_size);
//         serialized_key.SetLength(key_size);

//         if (err != CHIP_NO_ERROR) {
//             ESP_LOGI(TAG, "Generating new Root CA keypair");
//             if (m_root_ca_keypair.Initialize(chip::Crypto::ECPKeyTarget::ECDSA) != CHIP_NO_ERROR) {
//                 ESP_LOGE(TAG, "Failed to initialize Root CA keypair");
//                 return ESP_FAIL;
//             }
//             if (m_root_ca_keypair.Serialize(serialized_key) != CHIP_NO_ERROR) {
//                 ESP_LOGE(TAG, "Failed to serialize Root CA keypair");
//                 return ESP_FAIL;
//             }
//             if (storage.SyncSetKeyValue(kRootCAKeypairStorageKey, serialized_key.Bytes(),
//                                         static_cast<uint16_t>(serialized_key.Length())) != CHIP_NO_ERROR) {
//                 ESP_LOGE(TAG, "Failed to persist Root CA keypair");
//                 return ESP_FAIL;
//             }
//             ESP_LOGI(TAG, "Root CA keypair generated and persisted");
//         } else {
//             if (m_root_ca_keypair.Deserialize(serialized_key) != CHIP_NO_ERROR) {
//                 ESP_LOGE(TAG, "Failed to deserialize Root CA keypair");
//                 return ESP_FAIL;
//             }
//             ESP_LOGI(TAG, "Root CA keypair loaded from storage");
//         }

//         return ESP_OK;
//     }

//     chip::Controller::OperationalCredentialsDelegate *get_delegate() override { return this; }

//     // Called by setup_commissioner() to produce the controller's own NOC chain.
//     esp_err_t generate_controller_noc_chain(chip::NodeId node_id, chip::FabricId fabric_id,
//                                             chip::Crypto::P256Keypair &keypair, chip::MutableByteSpan &rcac,
//                                             chip::MutableByteSpan &icac, chip::MutableByteSpan &noc) override
//     {
//         if (keypair.Initialize(chip::Crypto::ECPKeyTarget::ECDSA) != CHIP_NO_ERROR) {
//             ESP_LOGE(TAG, "Failed to initialize controller keypair");
//             return ESP_FAIL;
//         }
//         CHIP_ERROR err = generate_noc_chain(node_id, fabric_id, chip::kUndefinedCATs, keypair.Pubkey(), rcac, icac, noc);
//         return err == CHIP_NO_ERROR ? ESP_OK : ESP_FAIL;
//     }

//     // Called by the commissioner during device commissioning (CSRResponse received).
//     // Extracts the device public key from csrElements TLV, signs a NOC, and
//     // invokes the completion callback synchronously.
//     CHIP_ERROR GenerateNOCChain(const chip::ByteSpan &csrElements,
//                                 const chip::ByteSpan &csrNonce,
//                                 const chip::ByteSpan &attestationSignature,
//                                 const chip::ByteSpan &attestationChallenge,
//                                 const chip::ByteSpan &DAC,
//                                 const chip::ByteSpan &PAI,
//                                 chip::Callback::Callback<chip::Controller::OnNOCChainGeneration> *onCompletion) override
//     {
//         // csrElements is TLV-encoded NocsrElements: { csr[1]: octet_string, csrNonce[2]: octet_string }
//         chip::TLV::ContiguousBufferTLVReader reader;
//         reader.Init(csrElements);
//         ReturnErrorOnFailure(reader.Next(chip::TLV::kTLVType_Structure, chip::TLV::AnonymousTag()));
//         chip::TLV::TLVType container;
//         ReturnErrorOnFailure(reader.EnterContainer(container));
//         ReturnErrorOnFailure(reader.Next(chip::TLV::ContextTag(1)));
//         chip::ByteSpan csr;
//         ReturnErrorOnFailure(reader.GetByteView(csr));

//         chip::Crypto::P256PublicKey device_pubkey;
//         ReturnErrorOnFailure(chip::Crypto::VerifyCertificateSigningRequest(csr.data(), csr.size(), device_pubkey));

//         uint64_t node_id = matter_controller_allocate_node_id();
//         ESP_LOGI(TAG, "Issuing NOC for device node 0x%llx", (unsigned long long)node_id);

//         uint8_t noc_buf[chip::Controller::kMaxCHIPDERCertLength];
//         uint8_t rcac_buf[chip::Controller::kMaxCHIPDERCertLength];
//         chip::MutableByteSpan noc_span(noc_buf);
//         chip::MutableByteSpan icac_span; // no intermediate CA
//         chip::MutableByteSpan rcac_span(rcac_buf);

//         ReturnErrorOnFailure(generate_noc_chain(node_id, kFabricId, chip::kUndefinedCATs,
//                                                 device_pubkey, rcac_span, icac_span, noc_span));

//         chip::Crypto::AesCcm128KeySpan ipk_span(s_ipk);
//         onCompletion->mCall(onCompletion->mContext, CHIP_NO_ERROR,
//                             noc_span, icac_span, rcac_span,
//                             chip::MakeOptional(ipk_span),
//                             chip::NullOptional);
//         return CHIP_NO_ERROR;
//     }

//     esp_err_t sign_device_noc(const chip::Crypto::P256PublicKey &device_pubkey, chip::NodeId node_id,
//                                chip::MutableByteSpan &noc, chip::MutableByteSpan &rcac)
//     {
//         chip::MutableByteSpan icac;
//         CHIP_ERROR err = generate_noc_chain(node_id, kFabricId, chip::kUndefinedCATs, device_pubkey, rcac, icac, noc);
//         return err == CHIP_NO_ERROR ? ESP_OK : ESP_FAIL;
//     }

// private:
//     CHIP_ERROR load_or_generate_rcac(chip::MutableByteSpan &rcac, chip::Credentials::ChipDN &rcac_dn)
//     {
//         uint16_t rcac_size = static_cast<uint16_t>(rcac.size());
//         CHIP_ERROR err     = m_storage->SyncGetKeyValue(kRootCACertStorageKey, rcac.data(), rcac_size);

//         if (err == CHIP_NO_ERROR) {
//             rcac.reduce_size(rcac_size);
//             return chip::Credentials::ExtractSubjectDNFromX509Cert(rcac, rcac_dn);
//         }

//         ReturnErrorOnFailure(rcac_dn.AddAttribute_MatterRCACId(kRootCAIssuerId));

//         chip::Credentials::X509CertRequestParams rcac_params;
//         rcac_params.SerialNumber  = 1;
//         rcac_params.ValidityStart = m_now;
//         rcac_params.ValidityEnd   = m_now + kCertValiditySeconds;
//         rcac_params.SubjectDN     = rcac_dn;
//         rcac_params.IssuerDN      = rcac_dn;

//         ESP_LOGI(TAG, "Generating Root CA certificate");
//         ReturnErrorOnFailure(chip::Credentials::NewRootX509Cert(rcac_params, m_root_ca_keypair, rcac));

//         rcac_dn = chip::Credentials::ChipDN{};
//         ReturnErrorOnFailure(chip::Credentials::ExtractSubjectDNFromX509Cert(rcac, rcac_dn));

//         ReturnErrorOnFailure(
//             m_storage->SyncSetKeyValue(kRootCACertStorageKey, rcac.data(), static_cast<uint16_t>(rcac.size())));
//         ESP_LOGI(TAG, "Root CA certificate generated and persisted");
//         return CHIP_NO_ERROR;
//     }

//     CHIP_ERROR generate_noc_chain(chip::NodeId node_id, chip::FabricId fabric_id, const chip::CATValues &cats,
//                                   const chip::Crypto::P256PublicKey &pubkey, chip::MutableByteSpan &rcac,
//                                   chip::MutableByteSpan &icac, chip::MutableByteSpan &noc)
//     {
//         chip::Credentials::ChipDN rcac_dn;
//         ReturnErrorOnFailure(load_or_generate_rcac(rcac, rcac_dn));

//         icac.reduce_size(0);

//         chip::Credentials::ChipDN noc_dn;
//         ReturnErrorOnFailure(noc_dn.AddAttribute_MatterFabricId(fabric_id));
//         ReturnErrorOnFailure(noc_dn.AddAttribute_MatterNodeId(node_id));
//         ReturnErrorOnFailure(noc_dn.AddCATs(cats));

//         chip::Credentials::X509CertRequestParams noc_params;
//         noc_params.SerialNumber  = 1;
//         noc_params.ValidityStart = m_now;
//         noc_params.ValidityEnd   = m_now + kCertValiditySeconds;
//         noc_params.SubjectDN     = noc_dn;
//         noc_params.IssuerDN      = rcac_dn;

//         return chip::Credentials::NewNodeOperationalX509Cert(noc_params, pubkey, m_root_ca_keypair, noc);
//     }

//     chip::PersistentStorageDelegate *m_storage = nullptr;
//     chip::Crypto::P256Keypair        m_root_ca_keypair;
//     uint32_t                         m_now     = 0;
// };

// static home_energy_manager_credentials_issuer s_credentials_issuer;

// ---------------------------------------------------------------------------
// IPK
// ---------------------------------------------------------------------------

// static esp_err_t load_or_generate_ipk(void)
// {
//     size_t read_size = 0;
//     CHIP_ERROR err   = chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Get(
//         kIPKStorageKey, s_ipk, kIPKLength, &read_size);

//     if (err == CHIP_NO_ERROR) {
//         ESP_LOGI(TAG, "IPK loaded from storage");
//         return ESP_OK;
//     }

//     ESP_LOGI(TAG, "Generating new IPK");
//     if (chip::Crypto::DRBG_get_bytes(s_ipk, kIPKLength) != CHIP_NO_ERROR) {
//         ESP_LOGE(TAG, "Failed to generate IPK");
//         return ESP_FAIL;
//     }
//     if (chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Put(kIPKStorageKey, s_ipk, kIPKLength) != CHIP_NO_ERROR) {
//         ESP_LOGE(TAG, "Failed to persist IPK");
//         return ESP_FAIL;
//     }
//     ESP_LOGI(TAG, "IPK generated and persisted");
//     return ESP_OK;
// }

// // ---------------------------------------------------------------------------
// // Public API
// // ---------------------------------------------------------------------------

// esp_err_t matter_controller_get_fabric_info(uint64_t *fabric_id_out, uint8_t *ipk_out, size_t ipk_buf_len)
// {
//     if (ipk_buf_len < kIPKLength) {
//         return ESP_ERR_INVALID_SIZE;
//     }
//     *fabric_id_out = kFabricId;
//     memcpy(ipk_out, s_ipk, kIPKLength);
//     return ESP_OK;
// }

uint64_t matter_controller_allocate_node_id(void)
{
    uint64_t node_id   = kFirstDeviceNodeId;
    size_t   read_size = sizeof(node_id);
    chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Get(kNodeIdCounterKey, &node_id, sizeof(node_id), &read_size);
    uint64_t next = node_id + 1;
    chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Put(kNodeIdCounterKey, &next, sizeof(next));
    return node_id;
}

// ---------------------------------------------------------------------------
// Node list (persisted in NVS as a blob of uint64_t values)
// ---------------------------------------------------------------------------

static void node_list_add(uint64_t node_id)
{
    uint64_t list[kMaxNodes] = {};
    size_t   read_size       = sizeof(list);
    size_t   count           = 0;

    if (chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Get(
            kNodeListKey, list, sizeof(list), &read_size) == CHIP_NO_ERROR) {
        count = read_size / sizeof(uint64_t);
    }

    if (count >= kMaxNodes) {
        ESP_LOGW(TAG, "Node list full, cannot add node 0x%llx", (unsigned long long)node_id);
        return;
    }
    list[count++] = node_id;
    chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Put(
        kNodeListKey, list, count * sizeof(uint64_t));
}

static void node_list_remove(uint64_t node_id)
{
    uint64_t list[kMaxNodes] = {};
    size_t   read_size       = sizeof(list);

    if (chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Get(
            kNodeListKey, list, sizeof(list), &read_size) != CHIP_NO_ERROR) {
        return;
    }
    size_t count = read_size / sizeof(uint64_t);
    size_t w     = 0;
    for (size_t i = 0; i < count; i++) {
        if (list[i] != node_id) list[w++] = list[i];
    }
    chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Put(
        kNodeListKey, list, w * sizeof(uint64_t));
}

esp_err_t matter_controller_get_nodes(uint64_t *nodes, size_t max, size_t *count_out)
{
    size_t read_size = max * sizeof(uint64_t);
    CHIP_ERROR err   = chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Get(
        kNodeListKey, nodes, read_size, &read_size);
    if (err == CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND) {
        *count_out = 0;
        return ESP_OK;
    }
    if (err != CHIP_NO_ERROR) {
        return ESP_FAIL;
    }
    *count_out = read_size / sizeof(uint64_t);
    return ESP_OK;
}

// esp_err_t matter_controller_sign_noc(const uint8_t *csr_der, size_t csr_der_len,
//                                      uint64_t node_id,
//                                      uint8_t *noc_out, size_t *noc_len,
//                                      uint8_t *rcac_out, size_t *rcac_len)
// {
//     chip::Crypto::P256PublicKey device_pubkey;
//     if (chip::Crypto::VerifyCertificateSigningRequest(csr_der, csr_der_len, device_pubkey) != CHIP_NO_ERROR) {
//         ESP_LOGE(TAG, "CSR verification failed");
//         return ESP_ERR_INVALID_ARG;
//     }

//     chip::MutableByteSpan noc_span(noc_out, *noc_len);
//     chip::MutableByteSpan rcac_span(rcac_out, *rcac_len);

//     chip::DeviceLayer::PlatformMgr().LockChipStack();
//     esp_err_t err = s_credentials_issuer.sign_device_noc(device_pubkey, node_id, noc_span, rcac_span);
//     chip::DeviceLayer::PlatformMgr().UnlockChipStack();

//     if (err != ESP_OK) {
//         ESP_LOGE(TAG, "NOC signing failed");
//         return err;
//     }

//     *noc_len  = noc_span.size();
//     *rcac_len = rcac_span.size();
//     return ESP_OK;
// }

// ---------------------------------------------------------------------------
// Blocking unpair
// ---------------------------------------------------------------------------

struct remove_node_ctx {
    SemaphoreHandle_t done;
    CHIP_ERROR        result;
};

static remove_node_ctx s_remove_ctx;

static void remove_node_cb(chip::NodeId remoteNodeId, CHIP_ERROR status)
{
    ESP_LOGI(TAG, "RemoveFabric complete for node 0x%llx: %" CHIP_ERROR_FORMAT,
             (unsigned long long)remoteNodeId, status.Format());
    if (status == CHIP_NO_ERROR) {
        node_list_remove(remoteNodeId);
    }
    s_remove_ctx.result = status;
    xSemaphoreGive(s_remove_ctx.done);
}

esp_err_t matter_controller_remove_node(uint64_t node_id)
{
    s_remove_ctx.done   = xSemaphoreCreateBinary();
    s_remove_ctx.result = CHIP_NO_ERROR;
    if (!s_remove_ctx.done) return ESP_ERR_NO_MEM;

    chip::DeviceLayer::PlatformMgr().LockChipStack();
    esp_err_t err = esp_matter::controller::matter_controller_client::get_instance()
                        .unpair((chip::NodeId)node_id, remove_node_cb);
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "unpair failed: 0x%x", err);
        vSemaphoreDelete(s_remove_ctx.done);
        return err;
    }

    if (xSemaphoreTake(s_remove_ctx.done, pdMS_TO_TICKS(30000)) != pdTRUE) {
        ESP_LOGE(TAG, "Remove node timed out");
        vSemaphoreDelete(s_remove_ctx.done);
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t result = (s_remove_ctx.result == CHIP_NO_ERROR) ? ESP_OK : ESP_FAIL;
    vSemaphoreDelete(s_remove_ctx.done);
    return result;
}

// ---------------------------------------------------------------------------
// Blocking on-network commissioning
// ---------------------------------------------------------------------------

class commission_pairing_delegate : public chip::Controller::DevicePairingDelegate
{
public:
    SemaphoreHandle_t done;
    CHIP_ERROR        result = CHIP_NO_ERROR;

    void OnCommissioningComplete(chip::NodeId nodeId, CHIP_ERROR error) override
    {
        ESP_LOGI(TAG, "OnCommissioningComplete node=0x%llx err=%" CHIP_ERROR_FORMAT,
                 (unsigned long long)nodeId, error.Format());
        result = error;
        xSemaphoreGive(done);
    }
};

esp_err_t matter_controller_commission_on_network(const char *onboarding_payload)
{
    // Parse the onboarding payload to extract PIN code and discriminator.
    chip::SetupPayload payload;
    CHIP_ERROR parse_err;

    // If it starts with MT: it's a QR code
    if (strncmp(onboarding_payload, "MT:", 3) == 0) {
        parse_err = chip::QRCodeSetupPayloadParser(onboarding_payload).populatePayload(payload);
    } else {
        parse_err = chip::ManualSetupPayloadParser(onboarding_payload).populatePayload(payload);
    }

    if (parse_err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Failed to parse onboarding payload: %" CHIP_ERROR_FORMAT, parse_err.Format());
        return ESP_ERR_INVALID_ARG;
    }

    commission_pairing_delegate delegate;
    delegate.done = xSemaphoreCreateBinary();
    if (!delegate.done) {
        return ESP_ERR_NO_MEM;
    }

    chip::NodeId node_id = matter_controller_allocate_node_id();

    //chip::RendezvousParameters rendezvous;
    //rendezvous.SetSetupPINCode(payload.setUpPINCode).SetDiscriminator(payload.discriminator.GetLongValue());

    // chip::DeviceLayer::PlatformMgr().LockChipStack();
    // auto *commissioner = esp_matter::controller::matter_controller_client::get_instance().get_commissioner();
    // commissioner->RegisterPairingDelegate(&delegate);
    // CHIP_ERROR err = commissioner->PairDevice(node_id, rendezvous);
    // chip::DeviceLayer::PlatformMgr().UnlockChipStack();

    // if (err != CHIP_NO_ERROR) {
    //     ESP_LOGE(TAG, "PairDevice failed: %" CHIP_ERROR_FORMAT, err.Format());
    //     vSemaphoreDelete(delegate.done);
    //     return ESP_FAIL;
    // }


    //esp_matter::controller::pairing_command_callbacks_t callbacks = {
    //    .commissioning_success_callback = on_commissioning_success_callback,
    //   .commissioning_failure_callback = on_commissioning_failure_callback};

    //heating_monitor::controller::pairing_command::get_instance().set_callbacks(callbacks);

    ESP_LOGI(TAG, "Attempting to commission node %luu", node_id);
    ESP_LOGI(TAG, "SetupCode %u", payload.setUpPINCode);
    ESP_LOGI(TAG, "Discriminator: %u", payload.discriminator.GetLongValue());  
    
    chip::DeviceLayer::PlatformMgr().LockChipStack();
    //esp_matter::controller::pairing_on_network(node_id, payload.setUpPINCode);
    esp_matter::controller::pairing_code(node_id, onboarding_payload);
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();

    // Block until OnCommissioningComplete fires (60 s timeout).
    const TickType_t timeout = pdMS_TO_TICKS(60000);
    if (xSemaphoreTake(delegate.done, timeout) != pdTRUE) {
        ESP_LOGE(TAG, "Commissioning timed out");
        //chip::DeviceLayer::PlatformMgr().LockChipStack();
        //commissioner->RegisterPairingDelegate(nullptr);
        //chip::DeviceLayer::PlatformMgr().UnlockChipStack();
        vSemaphoreDelete(delegate.done);
        return ESP_ERR_TIMEOUT;
    }

    //chip::DeviceLayer::PlatformMgr().LockChipStack();
    //commissioner->RegisterPairingDelegate(nullptr);
    //chip::DeviceLayer::PlatformMgr().UnlockChipStack();

    vSemaphoreDelete(delegate.done);
    if (delegate.result == CHIP_NO_ERROR) {
        node_list_add(node_id);
        return ESP_OK;
    }
    return ESP_FAIL;
}

// ---------------------------------------------------------------------------
// Startup
// ---------------------------------------------------------------------------

static void app_event_cb(const chip::DeviceLayer::ChipDeviceEvent *event, intptr_t)
{
    if (event->Type == chip::DeviceLayer::DeviceEventType::kCommissioningComplete) {
        ESP_LOGI(TAG, "kCommissioningComplete");
    }
}

esp_err_t matter_controller_start(void)
{
#if CONFIG_ENABLE_CHIP_SHELL
    esp_matter::console::diagnostics_register_commands();
    esp_matter::console::wifi_register_commands();
    esp_matter::console::factoryreset_register_commands();
    esp_matter::console::controller_register_commands();
    esp_matter::console::init();
#endif

    //esp_matter::controller::set_custom_credentials_issuer(&s_credentials_issuer);

    esp_err_t err = esp_matter::start(app_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_matter::start failed: 0x%x", err);
        return err;
    }

    // err = load_or_generate_ipk();
    // if (err != ESP_OK) {
    //     ESP_LOGE(TAG, "IPK init failed: 0x%x", err);
    //     return err;
    // }

    chip::DeviceLayer::PlatformMgr().LockChipStack();
    err = esp_matter::controller::matter_controller_client::get_instance().init(112233, 1, 5580);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Controller init failed: 0x%x", err);
        chip::DeviceLayer::PlatformMgr().UnlockChipStack();
        return err;
    }

    err = esp_matter::controller::matter_controller_client::get_instance().setup_commissioner();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Commissioner setup failed: 0x%x", err);
        chip::DeviceLayer::PlatformMgr().UnlockChipStack();
        return err;
    }

    chip::DeviceLayer::PlatformMgr().UnlockChipStack();

    ESP_LOGI(TAG, "Matter commissioner started");
    return ESP_OK;
}
