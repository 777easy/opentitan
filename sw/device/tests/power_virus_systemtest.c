// Copyright lowRISC contributors.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "hw/ip/aes/model/aes_modes.h"
#include "sw/device/lib/crypto/drivers/otbn.h"
#include "sw/device/lib/crypto/impl/rsa/rsa_3072_verify.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_macros.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"
#include "sw/ip/adc_ctrl/dif/dif_adc_ctrl.h"
#include "sw/ip/aes/dif/dif_aes.h"
#include "sw/ip/aes/test/utils/aes_testutils.h"
#include "sw/ip/csrng/dif/dif_csrng.h"
#include "sw/ip/csrng/dif/shared/dif_csrng_shared.h"
#include "sw/ip/edn/dif/dif_edn.h"
#include "sw/ip/entropy_src/test/utils/entropy_testutils.h"
#include "sw/ip/gpio/dif/dif_gpio.h"
#include "sw/ip/hmac/dif/dif_hmac.h"
#include "sw/ip/hmac/test/utils/hmac_testutils.h"
#include "sw/ip/i2c/dif/dif_i2c.h"
#include "sw/ip/i2c/test/utils/i2c_testutils.h"
#include "sw/ip/kmac/dif/dif_kmac.h"
#include "sw/ip/otbn/dif/dif_otbn.h"
#include "sw/ip/pinmux/dif/dif_pinmux.h"
#include "sw/ip/pinmux/test/utils/pinmux_testutils.h"
#include "sw/ip/rv_plic/dif/dif_rv_plic.h"
#include "sw/ip/spi_device/dif/dif_spi_device.h"
#include "sw/ip/spi_device/test/utils/spi_device_testutils.h"
#include "sw/ip/spi_host/dif/dif_spi_host.h"
#include "sw/lib/sw/device/base/math.h"
#include "sw/lib/sw/device/base/multibits.h"
#include "sw/lib/sw/device/runtime/log.h"

#include "adc_ctrl_regs.h"  // Generated.
#include "aes_regs.h"       // Generated.
#include "csrng_regs.h"     // Generated.
#include "gpio_regs.h"      // Generated.
#include "hmac_regs.h"      // Generated.
#include "hw/top_darjeeling/sw/autogen/top_darjeeling.h"
#include "i2c_regs.h"       // Generated.
#include "kmac_regs.h"      // Generated.
#include "spi_host_regs.h"  // Generated.

// The autogen rule that creates this header creates it in a directory named
// after the rule, then manipulates the include path in the
// cc_compilation_context to include that directory, so the compiler will find
// the version of this file matching the Bazel rule under test.
#include "rsa_3072_verify_testvectors.h"

OTTF_DEFINE_TEST_CONFIG(.enable_concurrency = true,
                        .enable_uart_flow_control = true);

/**
 * Peripheral DIF Handles.
 */
static dif_pinmux_t pinmux;
static dif_gpio_t gpio;
static dif_adc_ctrl_t adc_ctrl;
static dif_csrng_t csrng;
static dif_edn_t edn_0;
static dif_edn_t edn_1;
static dif_aes_t aes;
static dif_hmac_t hmac;
static dif_kmac_t kmac;
static dif_otbn_t otbn;
static dif_i2c_t i2c_0;
static dif_i2c_t i2c_1;
static dif_i2c_t i2c_2;
static dif_spi_device_handle_t spi_device;
static dif_spi_host_t spi_host_0;
static dif_spi_host_t spi_host_1;
static dif_rv_plic_t rv_plic;

static const dif_i2c_t *i2c_handles[] = {&i2c_0, &i2c_1, &i2c_2};
static dif_kmac_operation_state_t kmac_operation_state;

/**
 * Test configuration parameters.
 */
enum {
  /**
   * Test timeout parameter.
   */
  kTestTimeoutMicros = 1000,  // 1ms
  /**
   * ADC controller parameters.
   */
  kAdcCtrlPowerUpTimeAonCycles = 15,  // maximum power-up time
  /**
   * Entropy Source parameters.
   */
  kEntropySrcHealthTestWindowSize = 0x60,
  kEntropySrcAdaptiveProportionHealthTestHighThreshold = 0x50,
  kEntropySrcAdaptiveProportionHealthTestLowThreshold = 0x10,
  /**
   * EDN parameters.
   */
  kEdn0SeedMaterialNumWords = 0,
  kEdn1SeedMaterialNumWords = 12,
  kEdn0ReseedInterval = 128,
  kEdn1ReseedInterval = 32,
  /**
   * KMAC parameters.
   */
  kKmacEntropyReseedInterval = 1,
  kKmacEntropyHashThreshold = 1,  // KMAC operations between entropy requests
  kKmacEntropyWaitTimer = 0xffff,
  kKmacEntropyPrescaler = 0x3ff,
  kKmacMessageLength = 200,
  kKmacDigestLength = 16,
  /**
   * I2C parameters.
   */
  kI2cSclPeriodNs = 1000,
  kI2cSdaRiseFallTimeNs = 10,
  kI2cDeviceMask = 0x7f,
  kI2c0DeviceAddress0 = 0x11,
  kI2c0DeviceAddress1 = 0x22,
  kI2c1DeviceAddress0 = 0x33,
  kI2c1DeviceAddress1 = 0x44,
  kI2c2DeviceAddress0 = 0x55,
  kI2c2DeviceAddress1 = 0x66,
  kI2c0TargetAddress = 0x01,
  kI2c1TargetAddress = 0x02,
  kI2c2TargetAddress = 0x03,
  /**
   * SPI Host parameters.
   */
  // In chip.sv, only csid[0] is connected to a mio, the other wires
  // are fixed to 1'b1.
  kSpiHost1Csid = 0x0,
  kSpiHost1TxDataWord = 0xaaaaaaaa,
};

static uint32_t csrng_reseed_cmd_header;

/**
 * The peripheral clock of I2C IP (in nanoseconds)
 * In the DV sequence and in test_main(),
 * it is dynamically computed from kClockFreqPeripheralHz
 */
static volatile uint32_t peripheral_clock_period_ns;

/**
 * The mask share, used to mask kAesKey.
 */
static const uint8_t kAesKeyShare1[] = {
    0x0f, 0x1f, 0x2f, 0x3f, 0x4f, 0x5f, 0x6f, 0x7f, 0x8f, 0x9f, 0xaf,
    0xbf, 0xcf, 0xdf, 0xef, 0xff, 0x0a, 0x1a, 0x2a, 0x3a, 0x4a, 0x5a,
    0x6a, 0x7a, 0x8a, 0x9a, 0xaa, 0xba, 0xca, 0xda, 0xea, 0xfa,
};

static const dif_kmac_key_t kKmacKey = {
    .share0 = {0x43424140, 0x47464544, 0x4B4A4948, 0x4F4E4D4C, 0x53525150,
               0x57565554, 0x5B5A5958, 0x5F5E5D5C},
    .share1 = {0},
    .length = kDifKmacKeyLen256,
};

static const char *kKmacMessage =
    "\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f"
    "\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f"
    "\x20\x21\x22\x23\x24\x25\x26\x27\x28\x29\x2a\x2b\x2c\x2d\x2e\x2f"
    "\x30\x31\x32\x33\x34\x35\x36\x37\x38\x39\x3a\x3b\x3c\x3d\x3e\x3f"
    "\x40\x41\x42\x43\x44\x45\x46\x47\x48\x49\x4a\x4b\x4c\x4d\x4e\x4f"
    "\x50\x51\x52\x53\x54\x55\x56\x57\x58\x59\x5a\x5b\x5c\x5d\x5e\x5f"
    "\x60\x61\x62\x63\x64\x65\x66\x67\x68\x69\x6a\x6b\x6c\x6d\x6e\x6f"
    "\x70\x71\x72\x73\x74\x75\x76\x77\x78\x79\x7a\x7b\x7c\x7d\x7e\x7f"
    "\x80\x81\x82\x83\x84\x85\x86\x87\x88\x89\x8a\x8b\x8c\x8d\x8e\x8f"
    "\x90\x91\x92\x93\x94\x95\x96\x97\x98\x99\x9a\x9b\x9c\x9d\x9e\x9f"
    "\xa0\xa1\xa2\xa3\xa4\xa5\xa6\xa7\xa8\xa9\xaa\xab\xac\xad\xae\xaf"
    "\xb0\xb1\xb2\xb3\xb4\xb5\xb6\xb7\xb8\xb9\xba\xbb\xbc\xbd\xbe\xbf"
    "\xc0\xc1\xc2\xc3\xc4\xc5\xc6\xc7";

static const uint32_t kKmacDigest[] = {
    0xF71886B5, 0xD5E1921F, 0x558C1B6C, 0x18CDD7DD, 0xCAB4978B, 0x1E83994D,
    0x839A69B2, 0xD9E4A27D, 0xFDACFB70, 0xAE3300E5, 0xA2F185A5, 0xC3108570,
    0x0888072D, 0x2818BD01, 0x6847FE98, 0x6589FC76};

static rsa_3072_verify_test_vector_t rsa3072_test_vector;
static rsa_3072_int_t rsa3072_encoded_message;
static rsa_3072_constants_t rsa3072_constants;

static const uint8_t kI2cMessage[] = {
    0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
    0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
    0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
    0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
    0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
    0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
};

/**
 * External (OTTF) ISR override.
 */
void ottf_external_isr(void) {
  // Find which interrupt fired at PLIC by claiming it.
  dif_rv_plic_irq_id_t irq_id;
  CHECK_DIF_OK(
      dif_rv_plic_irq_claim(&rv_plic, kTopDarjeelingPlicTargetIbex0, &irq_id));

  top_darjeeling_plic_peripheral_t periph =
      top_darjeeling_plic_interrupt_for_peripheral[irq_id];
  switch (periph) {
    default:
      CHECK(false, "Unexpected IRQ fired with ID: %d", irq_id);
      break;
  }
}

/**
 * Initializes all DIF handles for each peripheral used in this test.
 */
static void init_peripheral_handles(void) {
  CHECK_DIF_OK(dif_adc_ctrl_init(
      mmio_region_from_addr(TOP_DARJEELING_ADC_CTRL_AON_BASE_ADDR), &adc_ctrl));
  CHECK_DIF_OK(
      dif_aes_init(mmio_region_from_addr(TOP_DARJEELING_AES_BASE_ADDR), &aes));
  CHECK_DIF_OK(dif_csrng_init(
      mmio_region_from_addr(TOP_DARJEELING_CSRNG_BASE_ADDR), &csrng));
  CHECK_DIF_OK(dif_edn_init(
      mmio_region_from_addr(TOP_DARJEELING_EDN0_BASE_ADDR), &edn_0));
  CHECK_DIF_OK(dif_edn_init(
      mmio_region_from_addr(TOP_DARJEELING_EDN1_BASE_ADDR), &edn_1));
  CHECK_DIF_OK(dif_hmac_init(
      mmio_region_from_addr(TOP_DARJEELING_HMAC_BASE_ADDR), &hmac));
  CHECK_DIF_OK(dif_gpio_init(
      mmio_region_from_addr(TOP_DARJEELING_GPIO_BASE_ADDR), &gpio));
  CHECK_DIF_OK(dif_kmac_init(
      mmio_region_from_addr(TOP_DARJEELING_KMAC_BASE_ADDR), &kmac));
  CHECK_DIF_OK(dif_pinmux_init(
      mmio_region_from_addr(TOP_DARJEELING_PINMUX_AON_BASE_ADDR), &pinmux));
  // UART 0 is already configured (and used) by the OTTF.
  CHECK_DIF_OK(dif_i2c_init(
      mmio_region_from_addr(TOP_DARJEELING_I2C0_BASE_ADDR), &i2c_0));
  CHECK_DIF_OK(dif_i2c_init(
      mmio_region_from_addr(TOP_DARJEELING_I2C1_BASE_ADDR), &i2c_1));
  CHECK_DIF_OK(dif_i2c_init(
      mmio_region_from_addr(TOP_DARJEELING_I2C2_BASE_ADDR), &i2c_2));
  CHECK_DIF_OK(dif_spi_device_init_handle(
      mmio_region_from_addr(TOP_DARJEELING_SPI_DEVICE_BASE_ADDR), &spi_device));
  CHECK_DIF_OK(dif_spi_host_init(
      mmio_region_from_addr(TOP_DARJEELING_SPI_HOST0_BASE_ADDR), &spi_host_0));
  CHECK_DIF_OK(dif_spi_host_init(
      mmio_region_from_addr(TOP_DARJEELING_SPI_HOST1_BASE_ADDR), &spi_host_1));
  CHECK_DIF_OK(dif_otbn_init(
      mmio_region_from_addr(TOP_DARJEELING_OTBN_BASE_ADDR), &otbn));
  CHECK_DIF_OK(dif_rv_plic_init(
      mmio_region_from_addr(TOP_DARJEELING_RV_PLIC_BASE_ADDR), &rv_plic));
}

static void configure_pinmux(void) {
  // Configure UART0 (console) and SW strapping pins.
  pinmux_testutils_init(&pinmux);

  // Configure GPIO max-power period indicator pin on IOB8.
  CHECK_DIF_OK(dif_pinmux_output_select(&pinmux, kTopDarjeelingPinmuxMioOutIob8,
                                        kTopDarjeelingPinmuxOutselGpioGpio0));

  // I2C0:
  //    SDA on IOA7
  //    SCL on IOA8
  CHECK_DIF_OK(dif_pinmux_input_select(&pinmux,
                                       kTopDarjeelingPinmuxPeripheralInI2c0Scl,
                                       kTopDarjeelingPinmuxInselIoa8));
  CHECK_DIF_OK(dif_pinmux_input_select(&pinmux,
                                       kTopDarjeelingPinmuxPeripheralInI2c0Sda,
                                       kTopDarjeelingPinmuxInselIoa7));
  CHECK_DIF_OK(dif_pinmux_output_select(&pinmux, kTopDarjeelingPinmuxMioOutIoa8,
                                        kTopDarjeelingPinmuxOutselI2c0Scl));
  CHECK_DIF_OK(dif_pinmux_output_select(&pinmux, kTopDarjeelingPinmuxMioOutIoa7,
                                        kTopDarjeelingPinmuxOutselI2c0Sda));

  // I2C1:
  //    SCL on IOB9
  //    SDA on IOB10
  CHECK_DIF_OK(dif_pinmux_input_select(&pinmux,
                                       kTopDarjeelingPinmuxPeripheralInI2c1Scl,
                                       kTopDarjeelingPinmuxInselIob9));
  CHECK_DIF_OK(dif_pinmux_input_select(&pinmux,
                                       kTopDarjeelingPinmuxPeripheralInI2c1Sda,
                                       kTopDarjeelingPinmuxInselIob10));
  CHECK_DIF_OK(dif_pinmux_output_select(&pinmux, kTopDarjeelingPinmuxMioOutIob9,
                                        kTopDarjeelingPinmuxOutselI2c1Scl));
  CHECK_DIF_OK(dif_pinmux_output_select(&pinmux,
                                        kTopDarjeelingPinmuxMioOutIob10,
                                        kTopDarjeelingPinmuxOutselI2c1Sda));

  // I2C2:
  //    SCL on IOB11
  //    SDA on IOB12
  CHECK_DIF_OK(dif_pinmux_input_select(&pinmux,
                                       kTopDarjeelingPinmuxPeripheralInI2c2Scl,
                                       kTopDarjeelingPinmuxInselIob11));
  CHECK_DIF_OK(dif_pinmux_input_select(&pinmux,
                                       kTopDarjeelingPinmuxPeripheralInI2c2Sda,
                                       kTopDarjeelingPinmuxInselIob12));
  CHECK_DIF_OK(dif_pinmux_output_select(&pinmux,
                                        kTopDarjeelingPinmuxMioOutIob11,
                                        kTopDarjeelingPinmuxOutselI2c2Scl));
  CHECK_DIF_OK(dif_pinmux_output_select(&pinmux,
                                        kTopDarjeelingPinmuxMioOutIob12,
                                        kTopDarjeelingPinmuxOutselI2c2Sda));

  // Apply this configuration only for the FPGA.
  // For the simulation, apply the config in configure_pinmux_sim().
  if (kDeviceType == kDeviceFpgaCw305 || kDeviceType == kDeviceFpgaCw310) {
    // SPI Host 1:
    //    CSB on IOB0
    //    SCK on IOB3
    //    SD0 on IOA2
    //    SD1 on IOR11
    //    SD2 on IOR12
    //    SD3 on IOR13
    CHECK_DIF_OK(
        dif_pinmux_output_select(&pinmux, kTopDarjeelingPinmuxMioOutIob0,
                                 kTopDarjeelingPinmuxOutselSpiHost1Csb));
    CHECK_DIF_OK(
        dif_pinmux_output_select(&pinmux, kTopDarjeelingPinmuxMioOutIob3,
                                 kTopDarjeelingPinmuxOutselSpiHost1Sck));
    CHECK_DIF_OK(
        dif_pinmux_output_select(&pinmux, kTopDarjeelingPinmuxMioOutIoa2,
                                 kTopDarjeelingPinmuxOutselSpiHost1Sd0));
    CHECK_DIF_OK(
        dif_pinmux_output_select(&pinmux, kTopDarjeelingPinmuxMioOutIor11,
                                 kTopDarjeelingPinmuxOutselSpiHost1Sd1));
    CHECK_DIF_OK(
        dif_pinmux_output_select(&pinmux, kTopDarjeelingPinmuxMioOutIor12,
                                 kTopDarjeelingPinmuxOutselSpiHost1Sd2));
    CHECK_DIF_OK(
        dif_pinmux_output_select(&pinmux, kTopDarjeelingPinmuxMioOutIor13,
                                 kTopDarjeelingPinmuxOutselSpiHost1Sd3));
    CHECK_DIF_OK(dif_pinmux_input_select(
        &pinmux, kTopDarjeelingPinmuxPeripheralInSpiHost1Sd0,
        kTopDarjeelingPinmuxInselIoa2));
    CHECK_DIF_OK(dif_pinmux_input_select(
        &pinmux, kTopDarjeelingPinmuxPeripheralInSpiHost1Sd1,
        kTopDarjeelingPinmuxInselIor11));
    CHECK_DIF_OK(dif_pinmux_input_select(
        &pinmux, kTopDarjeelingPinmuxPeripheralInSpiHost1Sd2,
        kTopDarjeelingPinmuxInselIor12));
    CHECK_DIF_OK(dif_pinmux_input_select(
        &pinmux, kTopDarjeelingPinmuxPeripheralInSpiHost1Sd3,
        kTopDarjeelingPinmuxInselIor13));
  }
}

/**
 * Configures pins for DVsim.
 * In chip_if.sv, agents and interfaces are connected to fixed pins.
 * To be able to use the agents (e.g, spi_device_agent1), the C code's pinmux
 * settings must be compatible with the settings in chip_if.sv.
 */
static void configure_pinmux_sim(void) {
  /**
   * Pinmux pad configurations for simulation.
   */
  const pinmux_pad_attributes_t pinmux_pad_attributes[] = {
      // Enable pull-ups for spi_host_0 data pins to avoid floating inputs.
      {
          .pad = kTopDarjeelingDirectPadsSpiHost0Sd0,
          .kind = kDifPinmuxPadKindDio,
          .flags = kDifPinmuxPadAttrPullResistorEnable |
                   kDifPinmuxPadAttrPullResistorUp,
      },
      {
          .pad = kTopDarjeelingDirectPadsSpiHost0Sd1,
          .kind = kDifPinmuxPadKindDio,
          .flags = kDifPinmuxPadAttrPullResistorEnable |
                   kDifPinmuxPadAttrPullResistorUp,
      },
      {
          .pad = kTopDarjeelingDirectPadsSpiHost0Sd2,
          .kind = kDifPinmuxPadKindDio,
          .flags = kDifPinmuxPadAttrPullResistorEnable |
                   kDifPinmuxPadAttrPullResistorUp,
      },
      {
          .pad = kTopDarjeelingDirectPadsSpiHost0Sd3,
          .kind = kDifPinmuxPadKindDio,
          .flags = kDifPinmuxPadAttrPullResistorEnable |
                   kDifPinmuxPadAttrPullResistorUp,
      },
      // Enable pull-ups for spi_host_1 data pins to avoid floating inputs.
      {
          .pad = kTopDarjeelingMuxedPadsIob3,  // SD0
          .kind = kDifPinmuxPadKindMio,
          .flags = kDifPinmuxPadAttrPullResistorEnable |
                   kDifPinmuxPadAttrPullResistorUp,
      },
      {
          .pad = kTopDarjeelingMuxedPadsIob4,  // SD1
          .kind = kDifPinmuxPadKindMio,
          .flags = kDifPinmuxPadAttrPullResistorEnable |
                   kDifPinmuxPadAttrPullResistorUp,
      },
      {
          .pad = kTopDarjeelingMuxedPadsIob5,  // SD2
          .kind = kDifPinmuxPadKindMio,
          .flags = kDifPinmuxPadAttrPullResistorEnable |
                   kDifPinmuxPadAttrPullResistorUp,
      },
      {
          .pad = kTopDarjeelingMuxedPadsIob6,  // SD3
          .kind = kDifPinmuxPadKindMio,
          .flags = kDifPinmuxPadAttrPullResistorEnable |
                   kDifPinmuxPadAttrPullResistorUp,
      },
  };

  // Enable pull-ups for SPI_HOST_0/1 data pins to avoid floating inputs.
  pinmux_testutils_configure_pads(&pinmux, pinmux_pad_attributes,
                                  ARRAYSIZE(pinmux_pad_attributes));

  // SPI Host 1 (from chip_if.sv):
  //    CSB on IOB1
  //    SCK on IOB0
  //    SD0 on IOB3
  //    SD1 on IOB4
  //    SD2 on IOB5
  //    SD3 on IOB6
  CHECK_DIF_OK(dif_pinmux_output_select(&pinmux, kTopDarjeelingPinmuxMioOutIob1,
                                        kTopDarjeelingPinmuxOutselSpiHost1Csb));
  CHECK_DIF_OK(dif_pinmux_output_select(&pinmux, kTopDarjeelingPinmuxMioOutIob0,
                                        kTopDarjeelingPinmuxOutselSpiHost1Sck));
  CHECK_DIF_OK(dif_pinmux_output_select(&pinmux, kTopDarjeelingPinmuxMioOutIob3,
                                        kTopDarjeelingPinmuxOutselSpiHost1Sd0));
  CHECK_DIF_OK(dif_pinmux_output_select(&pinmux, kTopDarjeelingPinmuxMioOutIob4,
                                        kTopDarjeelingPinmuxOutselSpiHost1Sd1));
  CHECK_DIF_OK(dif_pinmux_output_select(&pinmux, kTopDarjeelingPinmuxMioOutIob5,
                                        kTopDarjeelingPinmuxOutselSpiHost1Sd2));
  CHECK_DIF_OK(dif_pinmux_output_select(&pinmux, kTopDarjeelingPinmuxMioOutIob6,
                                        kTopDarjeelingPinmuxOutselSpiHost1Sd3));
  CHECK_DIF_OK(dif_pinmux_input_select(
      &pinmux, kTopDarjeelingPinmuxPeripheralInSpiHost1Sd0,
      kTopDarjeelingPinmuxInselIob3));
  CHECK_DIF_OK(dif_pinmux_input_select(
      &pinmux, kTopDarjeelingPinmuxPeripheralInSpiHost1Sd1,
      kTopDarjeelingPinmuxInselIob4));
  CHECK_DIF_OK(dif_pinmux_input_select(
      &pinmux, kTopDarjeelingPinmuxPeripheralInSpiHost1Sd2,
      kTopDarjeelingPinmuxInselIob5));
  CHECK_DIF_OK(dif_pinmux_input_select(
      &pinmux, kTopDarjeelingPinmuxPeripheralInSpiHost1Sd3,
      kTopDarjeelingPinmuxInselIob6));
}

/**
 * Configures adc_ctrl to continuously sample data (applying all filters across
 * both channels) in normal power mode, which is the most power intensive
 * sampling mode.
 */
static void configure_adc_ctrl_to_continuously_sample(void) {
  CHECK_DIF_OK(dif_adc_ctrl_configure(
      &adc_ctrl,
      (dif_adc_ctrl_config_t){
          .mode = kDifAdcCtrlNormalPowerScanMode,
          .power_up_time_aon_cycles = kAdcCtrlPowerUpTimeAonCycles,
          // Below configurations are unused, so set them to their reset
          // values.
          .wake_up_time_aon_cycles = ADC_CTRL_ADC_PD_CTL_WAKEUP_TIME_MASK,
          .num_low_power_samples = ADC_CTRL_ADC_LP_SAMPLE_CTL_REG_RESVAL,
          .num_normal_power_samples = ADC_CTRL_ADC_SAMPLE_CTL_REG_RESVAL,
      }));
  for (size_t filter = 0; filter < ADC_CTRL_PARAM_NUM_ADC_FILTER; ++filter) {
    for (size_t channel = 0; channel < ADC_CTRL_PARAM_NUM_ADC_CHANNEL;
         ++channel) {
      CHECK_DIF_OK(dif_adc_ctrl_configure_filter(
          &adc_ctrl, (dif_adc_ctrl_channel_t)channel,
          (dif_adc_ctrl_filter_config_t){
              .filter = (dif_adc_ctrl_filter_t)filter,
              // Set max range.
              .min_voltage = 0,
              .max_voltage = ADC_CTRL_ADC_CHN0_FILTER_CTL_0_MAX_V_0_MASK,
              .in_range = true,
              .generate_wakeup_on_match = false,
              .generate_irq_on_match = false,
          },
          kDifToggleEnabled));
    }
  }
}

static void configure_entropy_complex(void) {
  // The (test) ROM enables the entropy complex, and to reconfigure it
  // requires temporarily disabling it.
  CHECK_STATUS_OK(entropy_testutils_stop_all());

  // Configure CSRNG and create reseed command header for later use during
  // max power epoch.
  CHECK_DIF_OK(dif_csrng_configure(&csrng));
  csrng_reseed_cmd_header = csrng_cmd_header_build(
      kCsrngAppCmdReseed, kDifCsrngEntropySrcToggleEnable, /*cmd_len=*/0,
      /*generate_len=*/0);

  // Configure EDNs in auto mode.
  dif_edn_seed_material_t edn_empty_seed = {
      .len = kEdn0SeedMaterialNumWords,
  };
  dif_edn_seed_material_t edn_384_bit_seed = {
      .len = kEdn1SeedMaterialNumWords,
      .data = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12},
  };
  dif_edn_auto_params_t edn_auto_params = {
      .instantiate_cmd =
          {
              .cmd = csrng_cmd_header_build(kCsrngAppCmdInstantiate,
                                            kDifCsrngEntropySrcToggleEnable,
                                            /*cmd_len=*/edn_384_bit_seed.len,
                                            /*generate_len=*/0),
              .seed_material = edn_384_bit_seed,
          },
      .reseed_cmd =
          {
              .cmd = csrng_cmd_header_build(
                  kCsrngAppCmdReseed, kDifCsrngEntropySrcToggleEnable,
                  /*cmd_len=*/edn_384_bit_seed.len, /*generate_len=*/0),
              .seed_material = edn_384_bit_seed,
          },
      .generate_cmd =
          {
              .cmd = csrng_cmd_header_build(kCsrngAppCmdGenerate,
                                            kDifCsrngEntropySrcToggleEnable,
                                            /*cmd_len=*/0,
                                            /*generate_len=*/4096),
              .seed_material = edn_empty_seed,
          },
      .reseed_interval = 0,
  };
  // EDN0 provides lower-quality entropy. Let one generate command return
  // eight 128-bit blocks, and reseed every 128 generates.
  edn_auto_params.reseed_interval = kEdn0ReseedInterval;
  CHECK_DIF_OK(dif_edn_set_auto_mode(&edn_0, edn_auto_params));
  // EDN1 provides higher-quality entropy. Let one generate command return
  // eight 128-bit blocks, and reseed every 32 generates.
  edn_auto_params.reseed_interval = kEdn1ReseedInterval;
  CHECK_DIF_OK(dif_edn_set_auto_mode(&edn_1, edn_auto_params));
  CHECK_DIF_OK(dif_edn_configure(&edn_0));
  CHECK_DIF_OK(dif_edn_configure(&edn_1));
}

static status_t configure_aes(void) {
  // Prepare and load AES key shares.
  uint8_t aes_key_share0[sizeof(kAesModesKey256)];
  for (size_t i = 0; i < sizeof(kAesModesKey256); ++i) {
    aes_key_share0[i] = kAesModesKey256[i] ^ kAesKeyShare1[i];
  }
  dif_aes_key_share_t aes_key;
  memcpy(aes_key.share0, aes_key_share0, sizeof(aes_key.share0));
  memcpy(aes_key.share1, kAesKeyShare1, sizeof(aes_key.share1));

  // Prepare and load AES IV.
  dif_aes_iv_t aes_iv;
  memcpy(aes_iv.iv, kAesModesIvCbc, sizeof(aes_iv.iv));

  // Setup AES in automatic, 256-bit SW provided key, CBC encryption mode.
  // Additionally, we want to keep the entropy complex busing by constantly
  // reseeding PRNGs.
  dif_aes_transaction_t aes_transaction_cfg = {
      .operation = kDifAesOperationEncrypt,
      .mode = kDifAesModeCbc,
      .key_len = kDifAesKey256,
      .key_provider = kDifAesKeySoftwareProvided,
      .mask_reseeding = kDifAesReseedPerBlock,
      .manual_operation = kDifAesManualOperationManual,
      .reseed_on_key_change = false,
      .ctrl_aux_lock = false,
  };

  // Start the AES operation. Since we are in manual-mode, the encryption
  // will not start until plain text data is loaded into the appropriate
  // CSRs, and the encryption operation is triggered.
  AES_TESTUTILS_WAIT_FOR_STATUS(&aes, kDifAesStatusIdle, true,
                                kTestTimeoutMicros);
  CHECK_DIF_OK(dif_aes_start(&aes, &aes_transaction_cfg, &aes_key, &aes_iv));
  return OK_STATUS();
}

static void configure_hmac(void) {
  dif_hmac_transaction_t hmac_transaction_cfg = {
      .digest_endianness = kDifHmacEndiannessLittle,
      .message_endianness = kDifHmacEndiannessLittle,
  };
  // Use HMAC in SHA256 mode to generate a 256bit key from
  // `kHmacRefLongKey`.
  CHECK_DIF_OK(dif_hmac_mode_sha256_start(&hmac, hmac_transaction_cfg));
  CHECK_STATUS_OK(hmac_testutils_push_message(&hmac, (char *)kHmacRefLongKey,
                                              sizeof(kHmacRefLongKey)));
  CHECK_STATUS_OK(
      hmac_testutils_check_message_length(&hmac, sizeof(kHmacRefLongKey) * 8));
  CHECK_DIF_OK(dif_hmac_process(&hmac));
  dif_hmac_digest_t hmac_key_digest;
  CHECK_STATUS_OK(hmac_testutils_finish_polled(&hmac, &hmac_key_digest));

  // Configure the HMAC in HMAC mode.
  CHECK_DIF_OK(dif_hmac_mode_hmac_start(
      &hmac, (uint8_t *)&hmac_key_digest.digest[0], hmac_transaction_cfg));
}

static void configure_kmac(void) {
  dif_kmac_config_t kmac_cfg = (dif_kmac_config_t){
      .entropy_mode = kDifKmacEntropyModeEdn,
      .entropy_fast_process = kDifToggleDisabled,
      .entropy_hash_threshold = kKmacEntropyHashThreshold,
      .entropy_wait_timer = kKmacEntropyWaitTimer,
      .entropy_prescaler = kKmacEntropyPrescaler,
      .message_big_endian = false,
      .output_big_endian = false,
      .sideload = false,
      .msg_mask = false,
  };
  CHECK_DIF_OK(dif_kmac_configure(&kmac, kmac_cfg));
}

static void configure_i2c(dif_i2c_t *i2c, uint8_t device_addr_0,
                          uint8_t device_addr_1) {
  dif_i2c_config_t config;

  CHECK_DIF_OK(dif_i2c_compute_timing(
      (dif_i2c_timing_config_t){
          .lowest_target_device_speed = kDifI2cSpeedFastPlus,
          .clock_period_nanos = peripheral_clock_period_ns,
          .sda_rise_nanos = kI2cSdaRiseFallTimeNs,
          .sda_fall_nanos = kI2cSdaRiseFallTimeNs,
          .scl_period_nanos = kI2cSclPeriodNs},
      &config));
  CHECK_DIF_OK(dif_i2c_configure(i2c, config));
  dif_i2c_id_t id_0 = {.mask = kI2cDeviceMask, .address = device_addr_0};
  dif_i2c_id_t id_1 = {.mask = kI2cDeviceMask, .address = device_addr_1};
  CHECK_DIF_OK(dif_i2c_set_device_id(i2c, &id_0, &id_1));
  CHECK_DIF_OK(dif_i2c_line_loopback_set_enabled(i2c, kDifToggleEnabled));
}

static void configure_spi_host(const dif_spi_host_t *spi_host, bool enable) {
  CHECK(kClockFreqHiSpeedPeripheralHz <= UINT32_MAX,
        "kClockFreqHiSpeedPeripheralHz must fit in uint32_t");

  CHECK_DIF_OK(dif_spi_host_configure(
      spi_host,
      (dif_spi_host_config_t){
          .spi_clock = (uint32_t)kClockFreqHiSpeedPeripheralHz / 2,
          .peripheral_clock_freq_hz = (uint32_t)kClockFreqHiSpeedPeripheralHz,
          .chip_select =
              {
                  .idle = 2,
                  .trail = 2,
                  .lead = 2,
              },
      }));
  CHECK_DIF_OK(dif_spi_host_output_set_enabled(spi_host, /*enabled=*/enable));
  // dif_spi_host_configure() sets CTRL.SPIEN bit to true.
  // Adding this explicit control to be able set CTRL.SPIEN pin later
  // just before the max power epoch.
  mmio_region_write32(
      spi_host->base_addr, SPI_HOST_CONTROL_REG_OFFSET,
      bitfield_bit32_write(0, SPI_HOST_CONTROL_SPIEN_BIT, enable));
}

static void configure_otbn(void) {
  rsa3072_test_vector = rsa_3072_verify_tests[0];
  // Only one exponent (65537) is currently supported.
  CHECK(rsa3072_test_vector.publicKey.e == 65537);
  CHECK_STATUS_OK(rsa_3072_encode_sha256(rsa3072_test_vector.msg,
                                         rsa3072_test_vector.msgLen,
                                         &rsa3072_encoded_message));
  CHECK_STATUS_OK(rsa_3072_compute_constants(&rsa3072_test_vector.publicKey,
                                             &rsa3072_constants));
}

static void check_crypto_blocks_idle(void) {
  // CSRNG
  CHECK(mmio_region_get_bit32(csrng.base_addr, CSRNG_SW_CMD_STS_REG_OFFSET,
                              CSRNG_SW_CMD_STS_CMD_RDY_BIT));
  // AES
  CHECK(aes_testutils_get_status(&aes, kDifAesStatusIdle));
  // HMAC - no status register to check.
  // KMAC
  dif_kmac_status_t kmac_status;
  CHECK_DIF_OK(dif_kmac_get_status(&kmac, &kmac_status));
  CHECK(kmac_status.sha3_state == kDifKmacSha3StateAbsorbing);
  // OTBN
  dif_otbn_status_t otbn_status;
  CHECK_DIF_OK(dif_otbn_get_status(&otbn, &otbn_status));
  CHECK(otbn_status == kDifOtbnStatusIdle);
}

static void complete_kmac_operations(uint32_t *digest) {
  // Poll the status register until in the 'squeeze' state.
  CHECK_DIF_OK(dif_kmac_poll_status(&kmac, KMAC_STATUS_SHA3_SQUEEZE_BIT));

  // Read both shares of digest from state register and combine using XOR.
  ptrdiff_t digest_offset = KMAC_STATE_REG_OFFSET;
  for (size_t i = 0; i < kKmacDigestLength; ++i) {
    uint32_t share0 = mmio_region_read32(kmac.base_addr, digest_offset);
    uint32_t share1 = mmio_region_read32(
        kmac.base_addr, digest_offset + kDifKmacStateShareOffset);
    digest[i] = share0 ^ share1;
    digest_offset += sizeof(uint32_t);
  }
  kmac_operation_state.offset += kKmacDigestLength;

  // Complete KMAC operations and reset operation state.
  CHECK_DIF_OK(dif_kmac_end(&kmac, &kmac_operation_state));
}

/**
 * This function should be removed when we refactor the test to return
 * `status_t` to the ottf.
 */
static status_t aes_wait_for_status_ready(dif_aes_t *aes) {
  AES_TESTUTILS_WAIT_FOR_STATUS(aes, kDifAesStatusInputReady, true,
                                kTestTimeoutMicros);
  return OK_STATUS();
}

static void crypto_data_load_task(void *task_parameters) {
  LOG_INFO("Loading crypto block FIFOs with data ...");

  // Load data into AES block.
  dif_aes_data_t aes_plain_text;
  memcpy(aes_plain_text.data, kAesModesPlainText, sizeof(aes_plain_text.data));
  CHECK_STATUS_OK(aes_wait_for_status_ready(&aes));
  CHECK_DIF_OK(dif_aes_load_data(&aes, aes_plain_text));

  // Load data into HMAC block.
  CHECK_STATUS_OK(
      hmac_testutils_push_message(&hmac, kHmacRefData, sizeof(kHmacRefData)));
  CHECK_STATUS_OK(
      hmac_testutils_check_message_length(&hmac, sizeof(kHmacRefData) * 8));

  // Load data into KMAC block.
  dif_kmac_customization_string_t kmac_customization_string;
  CHECK_DIF_OK(dif_kmac_customization_string_init("My Tagged Application", 21,
                                                  &kmac_customization_string));
  CHECK_DIF_OK(dif_kmac_mode_kmac_start(
      &kmac, &kmac_operation_state, kDifKmacModeKmacLen256, kKmacDigestLength,
      &kKmacKey, &kmac_customization_string));
  CHECK_DIF_OK(dif_kmac_absorb(&kmac, &kmac_operation_state, kKmacMessage,
                               kKmacMessageLength, NULL));
  // Prepare KMAC for squeeze command (to come later in max power epoch) by
  // formatting message for KMAC operation. Note, below code is derived from
  // the KMAC DIF: `dif_kmac_sqeeze()`.
  CHECK(!kmac_operation_state.squeezing);
  if (kmac_operation_state.append_d) {
    // The KMAC operation requires that the output length (d) in bits be
    // right encoded and appended to the end of the message.
    uint32_t kmac_output_length_bits = kmac_operation_state.d * 32;
    int len = 1 + (kmac_output_length_bits > 0xFF) +
              (kmac_output_length_bits > 0xFFFF) +
              (kmac_output_length_bits > 0xFFFFFF);
    int shift = (len - 1) * 8;
    while (shift >= 8) {
      mmio_region_write8(kmac.base_addr, KMAC_MSG_FIFO_REG_OFFSET,
                         (uint8_t)(kmac_output_length_bits >> shift));
      shift -= 8;
    }
    mmio_region_write8(kmac.base_addr, KMAC_MSG_FIFO_REG_OFFSET,
                       (uint8_t)kmac_output_length_bits);
    mmio_region_write8(kmac.base_addr, KMAC_MSG_FIFO_REG_OFFSET, (uint8_t)len);
  }

  OTTF_TASK_DELETE_SELF_OR_DIE;
}

static void comms_data_load_task(void *task_parameters) {
  LOG_INFO("Loading communication block FIFOs with data ...");
  CHECK(ARRAYSIZE(kI2cMessage) == (I2C_PARAM_FIFO_DEPTH - 1));

  // Load data into I2C FIFOs.
  static_assert(ARRAYSIZE(i2c_handles) < UINT8_MAX,
                "Length of i2c_handles must fit in uint8_t");
  for (uint8_t i = 0; i < ARRAYSIZE(i2c_handles); ++i) {
    CHECK_STATUS_OK(i2c_testutils_write(i2c_handles[i], /*addr=*/i + 1,
                                        I2C_PARAM_FIFO_DEPTH - 1, kI2cMessage,
                                        /*skip_stop=*/false));
  }

  // Load data into SPI host (1; as 0 is used in passthrough mode) FIFO.
  uint32_t spi_host_tx_data[SPI_HOST_PARAM_TX_DEPTH];
  for (size_t i = 0; i < SPI_HOST_PARAM_TX_DEPTH; ++i) {
    spi_host_tx_data[i] = kSpiHost1TxDataWord;
  }
  dif_spi_host_segment_t spi_host_tx_segment = {
      .type = kDifSpiHostSegmentTypeTx,
      .tx = {
          .width = kDifSpiHostWidthQuad,
          .buf = (void *)&spi_host_tx_data,
          .length = ARRAYSIZE(spi_host_tx_data) * sizeof(uint32_t),
      }};
  CHECK_DIF_OK(dif_spi_host_transaction(&spi_host_1, kSpiHost1Csid,
                                        &spi_host_tx_segment, 1));

  OTTF_TASK_DELETE_SELF_OR_DIE;
}

static void max_power_task(void *task_parameters) {
  LOG_INFO("Starting the max power task ...");
  // ***************************************************************************
  // Trigger all chip operations.
  //
  // Note: We trigger the activations of each operation manually, rather
  // than use the DIFs, so that we can maximize the time overlap between all
  // operations.
  // ***************************************************************************

  // Prepare AES, HMAC, and KMAC trigger / process commands.
  uint32_t aes_trigger_reg = bitfield_bit32_write(0, kDifAesTriggerStart, true);
  uint32_t hmac_cmd_reg =
      mmio_region_read32(hmac.base_addr, HMAC_CMD_REG_OFFSET);
  hmac_cmd_reg =
      bitfield_bit32_write(hmac_cmd_reg, HMAC_CMD_HASH_PROCESS_BIT, true);
  uint32_t kmac_cmd_reg =
      bitfield_field32_write(0, KMAC_CMD_CMD_FIELD, KMAC_CMD_CMD_VALUE_PROCESS);

  // Prepare I2C, SPI host enablement commands (note, all configurations
  // between each IP instance should be configured the same).
  uint32_t i2c_ctrl_reg =
      mmio_region_read32(i2c_0.base_addr, I2C_CTRL_REG_OFFSET);
  i2c_ctrl_reg =
      bitfield_bit32_write(i2c_ctrl_reg, I2C_CTRL_ENABLEHOST_BIT, true);
  uint32_t spi_host_1_ctrl_reg =
      mmio_region_read32(spi_host_1.base_addr, SPI_HOST_CONTROL_REG_OFFSET);
  spi_host_1_ctrl_reg = bitfield_bit32_write(
      spi_host_1_ctrl_reg, SPI_HOST_CONTROL_OUTPUT_EN_BIT, true);
  spi_host_1_ctrl_reg = bitfield_bit32_write(spi_host_1_ctrl_reg,
                                             SPI_HOST_CONTROL_SPIEN_BIT, true);

  // Prepare adc_ctrl enablement command
  uint32_t adc_ctrl_reg =
      mmio_region_read32(adc_ctrl.base_addr, ADC_CTRL_ADC_EN_CTL_REG_OFFSET);
  adc_ctrl_reg = bitfield_bit32_write(adc_ctrl_reg,
                                      ADC_CTRL_ADC_EN_CTL_ADC_ENABLE_BIT, true);

  // Prepare GPIO register values (for max power indicator).
  const uint32_t gpio_on_reg_val = (1u << 16) | 1u;
  const uint32_t gpio_off_reg_val = 1u << 16;

  check_crypto_blocks_idle();

  LOG_INFO("Entering max power epoch ...");

  // Enable adc_ctrl
  mmio_region_write32(adc_ctrl.base_addr, ADC_CTRL_ADC_EN_CTL_REG_OFFSET,
                      adc_ctrl_reg);

  // Enable all I2Cs.
  mmio_region_write32(i2c_0.base_addr, I2C_CTRL_REG_OFFSET, i2c_ctrl_reg);
  mmio_region_write32(i2c_1.base_addr, I2C_CTRL_REG_OFFSET, i2c_ctrl_reg);
  mmio_region_write32(i2c_2.base_addr, I2C_CTRL_REG_OFFSET, i2c_ctrl_reg);

  // Issue OTBN start command.
  CHECK_STATUS_OK(rsa_3072_verify_start(&rsa3072_test_vector.signature,
                                        &rsa3072_test_vector.publicKey,
                                        &rsa3072_constants));

  // Enable SPI host (1).
  mmio_region_write32(spi_host_1.base_addr, SPI_HOST_CONTROL_REG_OFFSET,
                      spi_host_1_ctrl_reg);

  // Request entropy during max power epoch. Since AES is so fast,
  // realistically we will only be able to request a single block of
  // entropy.
  mmio_region_write32(csrng.base_addr, CSRNG_CMD_REQ_REG_OFFSET,
                      csrng_reseed_cmd_header);

  // Issue HMAC process and KMAC squeeze commands.
  mmio_region_write32(hmac.base_addr, HMAC_CMD_REG_OFFSET, hmac_cmd_reg);
  kmac_operation_state.squeezing = true;
  mmio_region_write32(kmac.base_addr, KMAC_CMD_REG_OFFSET, kmac_cmd_reg);

  // Toggle GPIO pin to indicate we are in max power consumption epoch.
  // Note, we do this BEFORE triggering the AES, since by the type the new
  // value propagates to the pin, the AES will already be active.
  mmio_region_write32(gpio.base_addr, GPIO_MASKED_OUT_LOWER_REG_OFFSET,
                      gpio_on_reg_val);

  // Issue AES trigger commands.
  mmio_region_write32(aes.base_addr, AES_TRIGGER_REG_OFFSET, aes_trigger_reg);

  // Wait for AES to complete encryption, as this is the fastest block.
  while (!(mmio_region_read32(aes.base_addr, AES_STATUS_REG_OFFSET) &
           (1u << AES_STATUS_OUTPUT_VALID_BIT)))
    ;

  // Toggle GPIO pin to indicate we are out of max power consumption epoch.
  mmio_region_write32(gpio.base_addr, GPIO_MASKED_OUT_LOWER_REG_OFFSET,
                      gpio_off_reg_val);

  LOG_INFO("Exited max power epoch.");

  // ***************************************************************************
  // Check operation results.
  // ***************************************************************************
  // Check AES operation.
  dif_aes_data_t aes_cipher_text;
  CHECK_DIF_OK(dif_aes_read_output(&aes, &aes_cipher_text));
  CHECK_ARRAYS_EQ((uint8_t *)aes_cipher_text.data, kAesModesCipherTextCbc256,
                  ARRAYSIZE(aes_cipher_text.data));

  // Check HMAC operations.
  CHECK_STATUS_OK(
      hmac_testutils_finish_and_check_polled(&hmac, &kHmacRefExpectedDigest));

  // Check KMAC operations.
  uint32_t kmac_digest[kKmacDigestLength];
  complete_kmac_operations(kmac_digest);
  CHECK(kKmacDigestLength == ARRAYSIZE(kKmacDigest));
  CHECK_ARRAYS_EQ(kmac_digest, kKmacDigest, ARRAYSIZE(kKmacDigest));

  // Check OTBN operations.
  hardened_bool_t result;
  CHECK_STATUS_OK(rsa_3072_verify_finalize(&rsa3072_encoded_message, &result));
  CHECK(result == kHardenedBoolTrue);

  // Check I2C bits TXed were echoed back by the DV agent. (Only for DV.)
  if (kDeviceType == kDeviceSimDV) {
    uint8_t fmt_fifo_lvl, rx_fifo_lvl;

    // Make sure all TX FIFOs have been drained.
    for (size_t ii = 0; ii < ARRAYSIZE(i2c_handles); ++ii) {
      do {
        CHECK_DIF_OK(dif_i2c_get_fifo_levels(i2c_handles[ii], &fmt_fifo_lvl,
                                             /*rx_fifo_lvl=*/NULL,
                                             /*tx_fifo_lvl=*/NULL,
                                             /*acq_fifo_lvl=*/NULL));
      } while (fmt_fifo_lvl > 0);
    };

    // Read data from I2C RX FIFO.
    static_assert(ARRAYSIZE(i2c_handles) < UINT8_MAX,
                  "Length of i2c_handles must fit in uint8_t");
    for (uint8_t ii = 0; ii < ARRAYSIZE(i2c_handles); ++ii) {
      CHECK_STATUS_OK(
          i2c_testutils_issue_read(i2c_handles[ii], /*addr=*/ii + 1,
                                   /*byte_count=*/I2C_PARAM_FIFO_DEPTH - 1));
    };

    // Make sure all data has been read back.
    for (size_t ii = 0; ii < ARRAYSIZE(i2c_handles); ++ii) {
      do {
        CHECK_DIF_OK(dif_i2c_get_fifo_levels(
            i2c_handles[ii], /*fmt_fifo_lvl=*/NULL, &rx_fifo_lvl,
            /*tx_fifo_lvl=*/NULL, /*acq_fifo_lvl=*/NULL));
      } while (rx_fifo_lvl < I2C_PARAM_FIFO_DEPTH - 1);
    };

    // Make sure read data is correct.
    for (size_t ii = 0; ii < ARRAYSIZE(i2c_handles); ++ii) {
      for (size_t jj = 0; jj < I2C_PARAM_FIFO_DEPTH - 1; ++jj) {
        uint8_t byte;
        CHECK_DIF_OK(dif_i2c_read_byte(i2c_handles[ii], &byte));
        CHECK(kI2cMessage[jj] == byte);
      };
    };
  }

  OTTF_TASK_DELETE_SELF_OR_DIE;
}

bool test_main(void) {
  peripheral_clock_period_ns =
      (uint32_t)udiv64_slow(1000000000, kClockFreqPeripheralHz, NULL);
  // Note: DO NOT change this message string without updating the DV
  // testbench.
  LOG_INFO("Computed peripheral clock period.");

  // ***************************************************************************
  // Initialize and configure all IPs.
  // ***************************************************************************
  init_peripheral_handles();
  configure_pinmux();
  // To be compatible with the configs in chip_if.sv,
  // apply the additional pinmux settings.
  if (kDeviceType == kDeviceSimDV || kDeviceType == kDeviceSimVerilator) {
    configure_pinmux_sim();
  }
  // Clear GPIO pin 0 (max power indicator pin).
  CHECK_DIF_OK(
      dif_gpio_output_set_enabled(&gpio, /*pin=*/0, kDifToggleEnabled));
  configure_adc_ctrl_to_continuously_sample();
  configure_entropy_complex();
  // Note: configuration of OTBN must be done *before* configuration of the
  // HMAC, as the cryptolib uses HMAC in SHA256 mode, which will cause HMAC
  // computation errors later in this test.
  configure_otbn();
  CHECK_STATUS_OK(configure_aes());
  configure_hmac();
  configure_kmac();
  configure_i2c(&i2c_0, kI2c0DeviceAddress0, kI2c0DeviceAddress1);
  configure_i2c(&i2c_1, kI2c1DeviceAddress0, kI2c1DeviceAddress1);
  configure_i2c(&i2c_2, kI2c2DeviceAddress0, kI2c2DeviceAddress1);
  configure_spi_host(&spi_host_0, /*enable=*/true);
  // We don't enable SPI host 1 just yet, as we want to pre-load its FIFO
  // with data before enabling it at the last moment, to initiate max power
  // draw.
  configure_spi_host(&spi_host_1, /*enable=*/false);
  CHECK_STATUS_OK(spi_device_testutils_configure_passthrough(
      &spi_device, /*filters=*/0,
      /*upload_write_commands=*/false));
  LOG_INFO("All IPs configured.");

  // ***************************************************************************
  // Kick off test tasks.
  // ***************************************************************************
  CHECK(ottf_task_create(crypto_data_load_task, "CryptoDataLoadTask",
                         kOttfFreeRtosMinStackSize, 1));
  CHECK(ottf_task_create(comms_data_load_task, "CommsDataLoadTask",
                         kOttfFreeRtosMinStackSize, 1));
  CHECK(ottf_task_create(max_power_task, "MaxPowerTask",
                         kOttfFreeRtosMinStackSize, 1));

  // ***************************************************************************
  // Yield control flow to the highest priority task in the run queue. Since
  // the tasks created above all have a higher priority level than the
  // current "test_main" task, and no tasks block, execution will not be
  // returned to the current task until the above tasks have been deleted.
  // ***************************************************************************
  LOG_INFO("Yielding execution to another task.");
  ottf_task_yield();

  return true;
}
