// Copyright lowRISC contributors.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#![allow(dead_code)]

///! Rust Top-Specific Definitions.
///!
///! This file contains const definitions for use within Rust code.

% for m in top["module"]:
  % if "memory" in m:
    % for key, val in m["memory"].items():
/// Memory base for ${m["name"]}_${val["label"]} in top ${top["name"]}.
pub const TOP_${top["name"].upper()}_${val["label"].upper()}_BASE_ADDR: usize = ${m["base_addrs"][key]};

/// Memory size for ${m["name"]}_${val["label"]} in top ${top["name"]}.
pub const TOP_${top["name"].upper()}_${val["label"].upper()}_SIZE_BYTES: usize = ${val["size"]};
    % endfor
  % endif
% endfor
% for m in top["memory"]:
/// Memory base address for ${m["name"]} in top ${top["name"]}.
pub const TOP_${top["name"].upper()}_${m["name"].upper()}_BASE_ADDR: usize = ${m["base_addr"]};

/// Memory size for ${m["name"]} in top ${top["name"]}.
pub const TOP_${top["name"].upper()}_${m["name"].upper()}_SIZE_BYTES: usize = ${m["size"]};
% endfor
% for (inst_name, if_name), region in helper.devices():
<%
    if_desc = inst_name if if_name is None else '{} device on {}'.format(if_name, inst_name)
    hex_base_addr = "0x{:X}".format(region.base_addr)
    hex_size_bytes = "0x{:X}".format(region.size_bytes)

    base_addr_name = region.base_addr_name().as_c_define()
    size_bytes_name = region.size_bytes_name().as_c_define()
%>\

/// Peripheral base address for ${if_desc} in top ${top["name"]}.
///
/// This should be used with #mmio_region_from_addr to access the memory-mapped
/// registers associated with the peripheral (usually via a DIF).
pub const ${base_addr_name}: usize = ${hex_base_addr};

/// Peripheral size for ${if_desc} in top ${top["name"]}.
///
/// This is the size (in bytes) of the peripheral's reserved memory area. All
/// memory-mapped registers associated with this peripheral should have an
/// address between #${base_addr_name} and
/// `${base_addr_name} + ${size_bytes_name}`.
pub const ${size_bytes_name}: usize = ${hex_size_bytes};
% endfor

/// MMIO Region
///
/// MMIO region excludes any memory that is separate from the module
/// configuration space, i.e. ROM, main SRAM, and flash are excluded but
/// retention SRAM, spi_device memory, or usbdev memory are included.
pub const ${helper.mmio.base_addr_name().as_c_define()}: usize = ${"0x{:X}".format(helper.mmio.base_addr)};
pub const ${helper.mmio.size_bytes_name().as_c_define()}: usize = ${"0x{:X}".format(helper.mmio.size_bytes)};
