#include "matter_controller.h"

#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_controller_client.h>
#include <esp_matter_controller_console.h>
#include <esp_matter_controller_credentials_issuer.h>

#include <controller/OperationalCredentialsDelegate.h>
#include <credentials/CHIPCert.h>
#include <crypto/CHIPCryptoPAL.h>
#include <lib/core/CASEAuthTag.h>
#include <lib/support/CHIPMem.h>
#include <lib/support/Span.h>
#include <protocols/secure_channel/PASESession.h>

static const char *TAG = "matter_controller";

static constexpr char     kRootCAKeypairStorageKey[] = "HEM_RootCAKey";
static constexpr char     kRootCACertStorageKey[]    = "HEM_RootCACert";
static constexpr uint32_t kCertValiditySeconds       = 10 * 365 * 24 * 3600;
static constexpr uint64_t kRootCAIssuerId            = 0xAB12AB12AB12AB12ULL;

class home_energy_manager_credentials_issuer
    : public esp_matter::controller::credentials_issuer
    , public chip::Controller::OperationalCredentialsDelegate
{
public:
    esp_err_t initialize_credentials_issuer(chip::PersistentStorageDelegate &storage) override
    {
        ESP_LOGI(TAG, "initialize_credentials_issuer");

        m_storage = &storage;

        chip::ASN1::ASN1UniversalTime effective_time;
        CHIP_ZERO_AT(effective_time);
        effective_time.Year  = 2021;
        effective_time.Month = 1;
        effective_time.Day   = 1;
        if (chip::Credentials::ASN1ToChipEpochTime(effective_time, m_now) != CHIP_NO_ERROR) {
            ESP_LOGE(TAG, "Failed to convert start time");
            return ESP_FAIL;
        }

        chip::Crypto::P256SerializedKeypair serialized_key;
        uint16_t key_size = static_cast<uint16_t>(serialized_key.Capacity());
        CHIP_ERROR err    = storage.SyncGetKeyValue(kRootCAKeypairStorageKey, serialized_key.Bytes(), key_size);
        serialized_key.SetLength(key_size);

        if (err != CHIP_NO_ERROR) {
            ESP_LOGI(TAG, "Generating new Root CA keypair");
            if (m_root_ca_keypair.Initialize(chip::Crypto::ECPKeyTarget::ECDSA) != CHIP_NO_ERROR) {
                ESP_LOGE(TAG, "Failed to initialize Root CA keypair");
                return ESP_FAIL;
            }
            if (m_root_ca_keypair.Serialize(serialized_key) != CHIP_NO_ERROR) {
                ESP_LOGE(TAG, "Failed to serialize Root CA keypair");
                return ESP_FAIL;
            }
            if (storage.SyncSetKeyValue(kRootCAKeypairStorageKey, serialized_key.Bytes(),
                                        static_cast<uint16_t>(serialized_key.Length())) != CHIP_NO_ERROR) {
                ESP_LOGE(TAG, "Failed to persist Root CA keypair");
                return ESP_FAIL;
            }
            ESP_LOGI(TAG, "Root CA keypair generated and persisted");
        } else {
            if (m_root_ca_keypair.Deserialize(serialized_key) != CHIP_NO_ERROR) {
                ESP_LOGE(TAG, "Failed to deserialize Root CA keypair");
                return ESP_FAIL;
            }
            ESP_LOGI(TAG, "Root CA keypair loaded from storage");
        }

        return ESP_OK;
    }

    chip::Controller::OperationalCredentialsDelegate *get_delegate() override { return this; }

    esp_err_t generate_controller_noc_chain(chip::NodeId node_id, chip::FabricId fabric_id,
                                            chip::Crypto::P256Keypair &keypair, chip::MutableByteSpan &rcac,
                                            chip::MutableByteSpan &icac, chip::MutableByteSpan &noc) override
    {
        CHIP_ERROR err = generate_noc_chain(node_id, fabric_id, chip::kUndefinedCATs, keypair.Pubkey(), rcac, icac, noc);
        return err == CHIP_NO_ERROR ? ESP_OK : ESP_FAIL;
    }

    // Commissioning is disabled; this pure virtual must be declared but will never be called.
    CHIP_ERROR GenerateNOCChain(const chip::ByteSpan &, const chip::ByteSpan &, const chip::ByteSpan &,
                                const chip::ByteSpan &, const chip::ByteSpan &, const chip::ByteSpan &,
                                chip::Callback::Callback<chip::Controller::OnNOCChainGeneration> *) override
    {
        return CHIP_ERROR_NOT_IMPLEMENTED;
    }

private:
    CHIP_ERROR load_or_generate_rcac(chip::MutableByteSpan &rcac, chip::Credentials::ChipDN &rcac_dn)
    {
        uint16_t rcac_size = static_cast<uint16_t>(rcac.size());
        CHIP_ERROR err     = m_storage->SyncGetKeyValue(kRootCACertStorageKey, rcac.data(), rcac_size);

        if (err == CHIP_NO_ERROR) {
            rcac.reduce_size(rcac_size);
            return chip::Credentials::ExtractSubjectDNFromX509Cert(rcac, rcac_dn);
        }

        // Build RCAC DN and generate a new self-signed Root CA certificate
        ReturnErrorOnFailure(rcac_dn.AddAttribute_MatterRCACId(kRootCAIssuerId));

        chip::Credentials::X509CertRequestParams rcac_params;
        rcac_params.SerialNumber  = 1;
        rcac_params.ValidityStart = m_now;
        rcac_params.ValidityEnd   = m_now + kCertValiditySeconds;
        rcac_params.SubjectDN     = rcac_dn;
        rcac_params.IssuerDN      = rcac_dn;

        ESP_LOGI(TAG, "Generating Root CA certificate");
        ReturnErrorOnFailure(chip::Credentials::NewRootX509Cert(rcac_params, m_root_ca_keypair, rcac));

        // Re-extract DN from the generated certificate
        rcac_dn = chip::Credentials::ChipDN{};
        ReturnErrorOnFailure(chip::Credentials::ExtractSubjectDNFromX509Cert(rcac, rcac_dn));

        ReturnErrorOnFailure(
            m_storage->SyncSetKeyValue(kRootCACertStorageKey, rcac.data(), static_cast<uint16_t>(rcac.size())));
        ESP_LOGI(TAG, "Root CA certificate generated and persisted");
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR generate_noc_chain(chip::NodeId node_id, chip::FabricId fabric_id, const chip::CATValues &cats,
                                  const chip::Crypto::P256PublicKey &pubkey, chip::MutableByteSpan &rcac,
                                  chip::MutableByteSpan &icac, chip::MutableByteSpan &noc)
    {
        chip::Credentials::ChipDN rcac_dn;
        ReturnErrorOnFailure(load_or_generate_rcac(rcac, rcac_dn));

        // Sign the NOC directly from the Root CA (no intermediate CA)
        icac.reduce_size(0);

        chip::Credentials::ChipDN noc_dn;
        ReturnErrorOnFailure(noc_dn.AddAttribute_MatterFabricId(fabric_id));
        ReturnErrorOnFailure(noc_dn.AddAttribute_MatterNodeId(node_id));
        ReturnErrorOnFailure(noc_dn.AddCATs(cats));

        chip::Credentials::X509CertRequestParams noc_params;
        noc_params.SerialNumber  = 1;
        noc_params.ValidityStart = m_now;
        noc_params.ValidityEnd   = m_now + kCertValiditySeconds;
        noc_params.SubjectDN     = noc_dn;
        noc_params.IssuerDN      = rcac_dn;

        return chip::Credentials::NewNodeOperationalX509Cert(noc_params, pubkey, m_root_ca_keypair, noc);
    }

    chip::PersistentStorageDelegate *m_storage = nullptr;
    chip::Crypto::P256Keypair        m_root_ca_keypair;
    uint32_t                         m_now     = 0;
};

static home_energy_manager_credentials_issuer s_credentials_issuer;

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
#endif // CONFIG_ENABLE_CHIP_SHELL

    esp_matter::controller::set_custom_credentials_issuer(&s_credentials_issuer);

    esp_err_t err = esp_matter::start(app_event_cb);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_matter::start failed: 0x%x", err);
        return err;
    }

    auto &controller = esp_matter::controller::matter_controller_client::get_instance();

    chip::DeviceLayer::PlatformMgr().LockChipStack();
    err = controller.init(chip::kTestControllerNodeId, /* fabric_id */ 1, /* listen_port */ 5580);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Controller init failed: 0x%x", err);
        return err;
    }
    
    chip::MutableByteSpan empty_ipk;
    err = controller.setup_controller(empty_ipk);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Controller setup failed: 0x%x", err);
        return err;
    }

    chip::DeviceLayer::PlatformMgr().UnlockChipStack();

    ESP_LOGI(TAG, "Matter controller started");

    return ESP_OK;
}
