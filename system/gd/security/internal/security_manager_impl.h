/*
 * Copyright 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <unordered_map>
#include <utility>

#include "hci/acl_manager.h"
#include "l2cap/classic/security_enforcement_interface.h"
#include "l2cap/le/l2cap_le_module.h"
#include "l2cap/le/security_enforcement_interface.h"
#include "os/handler.h"
#include "security/channel/security_manager_channel.h"
#include "security/initial_informations.h"
#include "security/pairing/classic_pairing_handler.h"
#include "security/pairing_handler_le.h"
#include "security/record/security_record.h"
#include "security/record/security_record_database.h"

namespace bluetooth {
namespace security {

class ISecurityManagerListener;

static constexpr hci::IoCapability kDefaultIoCapability = hci::IoCapability::DISPLAY_YES_NO;
static constexpr hci::OobDataPresent kDefaultOobDataPresent = hci::OobDataPresent::NOT_PRESENT;
static constexpr hci::AuthenticationRequirements kDefaultAuthenticationRequirements =
    hci::AuthenticationRequirements::GENERAL_BONDING;

namespace internal {

struct LeFixedChannelEntry {
  std::unique_ptr<l2cap::le::FixedChannel> channel_;
  std::unique_ptr<os::EnqueueBuffer<packet::BasePacketBuilder>> enqueue_buffer_;
};

class SecurityManagerImpl : public channel::ISecurityManagerChannelListener, public UICallbacks {
 public:
  explicit SecurityManagerImpl(
      os::Handler* security_handler,
      l2cap::le::L2capLeModule* l2cap_le_module,
      channel::SecurityManagerChannel* security_manager_channel,
      hci::HciLayer* hci_layer,
      hci::AclManager* acl_manager);

  ~SecurityManagerImpl() {
    /* L2CAP layer doesn't guarantee to send the registered OnCloseCallback during shutdown. Cleanup the remaining
     * queues to prevent crashes */
    for (auto& stored_chan : all_channels_) {
      stored_chan.channel_->GetQueueUpEnd()->UnregisterDequeue();
      stored_chan.enqueue_buffer_.reset();
    }
  }

  // All APIs must be invoked in SM layer handler

  /**
   * Initialize the security record map from an internal device database.
   */
  void Init();

  /**
   * Initiates bond over Classic transport with device, if not bonded yet.
   *
   * @param address device address we want to bond with
   */
  void CreateBond(hci::AddressWithType address);

  /**
   * Initiates bond over Low Energy transport with device, if not bonded yet.
   *
   * @param address device address we want to bond with
   */
  void CreateBondLe(hci::AddressWithType address);

  /* void CreateBond(std::shared_ptr<hci::LeDevice> device); */

  /**
   * Cancels the pairing process for this device.
   *
   * @param device pointer to device with which we want to cancel our bond
   */
  void CancelBond(hci::AddressWithType device);

  /* void CancelBond(std::shared_ptr<hci::LeDevice> device); */

  /**
   * Disassociates the device and removes the persistent LTK
   *
   * @param device pointer to device we want to forget
   * @return true if removed
   */
  void RemoveBond(hci::AddressWithType device);

  /* void RemoveBond(std::shared_ptr<hci::LeDevice> device); */

  /**
   * Register Security UI handler, for handling prompts around the Pairing process.
   */
  void SetUserInterfaceHandler(UI* user_interface, os::Handler* handler);

  /**
   * Specify the initiator address policy used for LE transport. Can only be called once.
   */
  void SetLeInitiatorAddressPolicyForTest(
      hci::LeAddressManager::AddressPolicy address_policy,
      hci::AddressWithType fixed_address,
      crypto_toolbox::Octet16 rotation_irk,
      std::chrono::milliseconds minimum_rotation_time,
      std::chrono::milliseconds maximum_rotation_time);

  /**
   * Register to listen for callback events from SecurityManager
   *
   * @param listener ISecurityManagerListener instance to handle callbacks
   */
  void RegisterCallbackListener(ISecurityManagerListener* listener, os::Handler* handler);

  /**
   * Unregister listener for callback events from SecurityManager
   *
   * @param listener ISecurityManagerListener instance to unregister
   */
  void UnregisterCallbackListener(ISecurityManagerListener* listener);

  /**
   * Handle the events sent back from HCI that we care about
   *
   * @param packet data received from HCI
   */
  void OnHciEventReceived(hci::EventPacketView packet) override;

  /**
   * When a conncetion closes we should clean up the pairing handler
   *
   * @param address Remote address
   */
  void OnConnectionClosed(hci::Address address) override;

  /**
   * Pairing handler has finished or cancelled
   *
   * @param address address for pairing handler
   * @param status status from SimplePairingComplete or other error code
   */
  void OnPairingHandlerComplete(hci::Address address, PairingResultOrFailure status);

  // UICallbacks implementation
  void OnPairingPromptAccepted(const bluetooth::hci::AddressWithType& address, bool confirmed) override;
  void OnConfirmYesNo(const bluetooth::hci::AddressWithType& address, bool confirmed) override;
  void OnPasskeyEntry(const bluetooth::hci::AddressWithType& address, uint32_t passkey) override;

  // Facade Configuration API functions
  void SetIoCapability(hci::IoCapability io_capability);
  void SetAuthenticationRequirements(hci::AuthenticationRequirements authentication_requirements);
  void SetOobDataPresent(hci::OobDataPresent data_present);
  void SetLeIoCapability(security::IoCapability io_capability);
  void SetLeAuthRequirements(uint8_t auth_req);
  void SetLeOobDataPresent(OobDataFlag data_present);
  void GetOutOfBandData(std::array<uint8_t, 16>* le_sc_confirmation_value, std::array<uint8_t, 16>* le_sc_random_value);
  void SetOutOfBandData(
      hci::AddressWithType remote_address,
      std::array<uint8_t, 16> le_sc_confirmation_value,
      std::array<uint8_t, 16> le_sc_random_value);

  void EnforceSecurityPolicy(hci::AddressWithType remote, l2cap::classic::SecurityPolicy policy,
                             l2cap::classic::SecurityEnforcementInterface::ResultCallback result_callback);
  void EnforceLeSecurityPolicy(hci::AddressWithType remote, l2cap::le::SecurityPolicy policy,
                               l2cap::le::SecurityEnforcementInterface::ResultCallback result_callback);
 protected:
  std::vector<std::pair<ISecurityManagerListener*, os::Handler*>> listeners_;
  UI* user_interface_ = nullptr;
  os::Handler* user_interface_handler_ = nullptr;

  void NotifyDeviceBonded(hci::AddressWithType device);
  void NotifyDeviceBondFailed(hci::AddressWithType device, PairingResultOrFailure status);
  void NotifyDeviceUnbonded(hci::AddressWithType device);
  void NotifyEncryptionStateChanged(hci::EncryptionChangeView encryption_change_view);

 private:
  template <class T>
  void HandleEvent(T packet);

  void DispatchPairingHandler(std::shared_ptr<record::SecurityRecord> record, bool locally_initiated);
  void OnL2capRegistrationCompleteLe(l2cap::le::FixedChannelManager::RegistrationResult result,
                                     std::unique_ptr<l2cap::le::FixedChannelService> le_smp_service);
  void OnSmpCommandLe(hci::AddressWithType device);
  void OnConnectionOpenLe(std::unique_ptr<l2cap::le::FixedChannel> channel);
  void OnConnectionClosedLe(hci::AddressWithType address, hci::ErrorCode error_code);
  void OnConnectionFailureLe(bluetooth::l2cap::le::FixedChannelManager::ConnectionResult result);
  void OnPairingFinished(bluetooth::security::PairingResultOrFailure pairing_result);
  void OnHciLeEvent(hci::LeMetaEventView event);
  LeFixedChannelEntry* FindStoredLeChannel(const hci::AddressWithType& device);
  bool EraseStoredLeChannel(const hci::AddressWithType& device);
  void InternalEnforceSecurityPolicy(
      hci::AddressWithType remote,
      l2cap::classic::SecurityPolicy policy,
      l2cap::classic::SecurityEnforcementInterface::ResultCallback result_callback,
      bool try_meet_requirements);
  void ConnectionIsReadyStartPairing(LeFixedChannelEntry* stored_channel);
  void WipeLePairingHandler();

  os::Handler* security_handler_ __attribute__((unused));
  l2cap::le::L2capLeModule* l2cap_le_module_ __attribute__((unused));
  std::unique_ptr<l2cap::le::FixedChannelManager> l2cap_manager_le_;
  hci::LeSecurityInterface* hci_security_interface_le_ __attribute__((unused));
  channel::SecurityManagerChannel* security_manager_channel_;
  hci::AclManager* acl_manager_;
  record::SecurityRecordDatabase security_database_;
  std::unordered_map<hci::Address, std::shared_ptr<pairing::PairingHandler>> pairing_handler_map_;
  hci::IoCapability local_io_capability_ = kDefaultIoCapability;
  hci::AuthenticationRequirements local_authentication_requirements_ = kDefaultAuthenticationRequirements;
  hci::OobDataPresent local_oob_data_present_ = kDefaultOobDataPresent;
  security::IoCapability local_le_io_capability_ = security::IoCapability::NO_INPUT_NO_OUTPUT;
  uint8_t local_le_auth_req_ = AuthReqMaskBondingFlag | AuthReqMaskMitm | AuthReqMaskSc;
  OobDataFlag local_le_oob_data_present_ = OobDataFlag::NOT_PRESENT;
  std::optional<MyOobData> local_le_oob_data_;
  std::optional<hci::AddressWithType> remote_oob_data_address_;
  std::optional<crypto_toolbox::Octet16> remote_oob_data_le_sc_c_;
  std::optional<crypto_toolbox::Octet16> remote_oob_data_le_sc_r_;

  std::unordered_map<
      hci::AddressWithType,
      std::pair<l2cap::classic::SecurityPolicy, l2cap::classic::SecurityEnforcementInterface::ResultCallback>>
      enforce_security_policy_callback_map_;

  struct {
    hci::AddressWithType address_;
    uint16_t connection_handle_;
    std::unique_ptr<PairingHandlerLe> handler_;
  } pending_le_pairing_;

  std::list<LeFixedChannelEntry> all_channels_;
};
}  // namespace internal
}  // namespace security
}  // namespace bluetooth
