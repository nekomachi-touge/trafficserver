/** @file
 *
 *  QUIC Packet Header Protector
 *
 *  @section license License
 *
 *  Licensed to the Apache Software Foundation (ASF) under one
 *  or more contributor license agreements.  See the NOTICE file
 *  distributed with this work for additional information
 *  regarding copyright ownership.  The ASF licenses this file
 *  to you under the Apache License, Version 2.0 (the
 *  "License"); you may not use this file except in compliance
 *  with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include "QUICPacketHeaderProtector.h"
#include "QUICDebugNames.h"
#include "QUICPacket.h"

#include "tscore/Diags.h"

bool
QUICPacketHeaderProtector::protect(uint8_t *protected_pn, uint8_t &protected_pn_len, const uint8_t *unprotected_pn,
                                   uint8_t unprotected_pn_len, const uint8_t *sample, QUICKeyPhase phase) const
{
  const QUIC_EVP_CIPHER *aead = this->_hs_protocol->cipher_for_hp(phase);

  const KeyMaterial *km = this->_hs_protocol->key_material_for_encryption(phase);
  if (!km) {
    return false;
  }

  bool ret =
    this->_encrypt_pn(protected_pn, protected_pn_len, unprotected_pn, unprotected_pn_len, sample, km->hp, km->hp_len, aead);
  if (!ret) {
    Debug("quic_pne", "Failed to encrypt a packet number");
  }
  return ret;
}

bool
QUICPacketHeaderProtector::unprotect(uint8_t *protected_packet, size_t protected_packet_len)
{
  // Do nothing if the packet is VN
  if (QUICInvariants::is_long_header(protected_packet)) {
    QUICVersion version;
    QUICPacketLongHeader::version(version, protected_packet, protected_packet_len);
    if (version == 0x0) {
      return true;
    }
  }

  QUICKeyPhase phase;
  QUICPacketType type;
  if (QUICInvariants::is_long_header(protected_packet)) {
    QUICPacketLongHeader::key_phase(phase, protected_packet, protected_packet_len);
    QUICPacketLongHeader::type(type, protected_packet, protected_packet_len);
  } else {
    QUICPacketShortHeader::key_phase(phase, protected_packet, protected_packet_len);
    type = QUICPacketType::PROTECTED;
  }

  Debug("v_quic_pne", "Unprotecting a packet number of %s packet using %s", QUICDebugNames::packet_type(type),
        QUICDebugNames::key_phase(phase));

  const QUIC_EVP_CIPHER *aead = this->_hs_protocol->cipher_for_hp(phase);
  if (!aead) {
    Debug("quic_pne", "Failed to decrypt a packet number: keys for %s is not ready", QUICDebugNames::key_phase(phase));
    return false;
  }

  const KeyMaterial *km = this->_hs_protocol->key_material_for_decryption(phase);
  if (!km) {
    Debug("quic_pne", "Failed to decrypt a packet number: keys for %s is not ready", QUICDebugNames::key_phase(phase));
    return false;
  }

  uint8_t sample_offset;
  if (!this->_calc_sample_offset(&sample_offset, protected_packet, protected_packet_len)) {
    Debug("v_quic_pne", "Failed to calculate a sample offset");
    return false;
  }

  uint8_t mask[EVP_MAX_BLOCK_LENGTH];
  if (!this->_generate_mask(mask, protected_packet + sample_offset, km->hp, aead)) {
    Debug("v_quic_pne", "Failed to generate a mask");
    return false;
  }

  if (!this->_unprotect(protected_packet, protected_packet_len, mask)) {
    Debug("quic_pne", "Failed to decrypt a packet number");
  }

  return true;
}

void
QUICPacketHeaderProtector::set_hs_protocol(const QUICHandshakeProtocol *hs_protocol)
{
  this->_hs_protocol = hs_protocol;
}

bool
QUICPacketHeaderProtector::_calc_sample_offset(uint8_t *sample_offset, const uint8_t *protected_packet, size_t protected_packet_len)
{
  uint8_t pn_offset      = 0;
  uint8_t aead_expansion = 16;
  QUICKeyPhase phase;

  if (QUICInvariants::is_long_header(protected_packet)) {
    uint8_t dcil;
    uint8_t scil;
    size_t dummy;
    uint8_t length_len;
    QUICPacketLongHeader::dcil(dcil, protected_packet, protected_packet_len);
    QUICPacketLongHeader::scil(scil, protected_packet, protected_packet_len);
    QUICPacketLongHeader::length(dummy, &length_len, protected_packet, protected_packet_len);
    *sample_offset = 6 + dcil + scil + length_len + 4;

    QUICPacketType type;
    QUICPacketLongHeader::type(type, protected_packet, protected_packet_len);
    if (type == QUICPacketType::INITIAL) {
      size_t token_len;
      uint8_t token_length_len;
      QUICPacketLongHeader::token_length(token_len, &token_length_len, protected_packet, protected_packet_len);
      *sample_offset += token_len + token_length_len;
    }
  } else {
    QUICPacketShortHeader::key_phase(phase, protected_packet, protected_packet_len);
  }
  return std::min(static_cast<size_t>(pn_offset) + 4, protected_packet_len - aead_expansion);
}

bool
QUICPacketHeaderProtector::_unprotect(uint8_t *protected_packet, size_t protected_packet_len, const uint8_t *mask)
{
  uint8_t pn_offset;

  // Unprotect packet number
  if (QUICInvariants::is_long_header(protected_packet)) {
    protected_packet[0] ^= mask[0] & 0x0f;
    QUICPacketLongHeader::packet_number_offset(pn_offset, protected_packet, protected_packet_len);
  } else {
    protected_packet[0] ^= mask[0] & 0x1f;
    QUICPacketShortHeader::packet_number_offset(pn_offset, protected_packet, protected_packet_len, QUICConnectionId::SCID_LEN);
  }
  uint8_t pn_length = QUICTypeUtil::read_QUICPacketNumberLen(protected_packet);

  for (int i = 0; i < pn_length; ++i) {
    protected_packet[pn_offset + i] ^= mask[1 + i];
  }

  return true;
}
