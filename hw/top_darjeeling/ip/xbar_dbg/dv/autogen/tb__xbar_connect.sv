// Copyright lowRISC contributors.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// tb__xbar_connect generated by `tlgen.py` tool

xbar_dbg dut();

`DRIVE_CLK(clk_dbg_i)

initial force dut.clk_dbg_i = clk_dbg_i;

// TODO, all resets tie together
initial force dut.rst_dbg_ni = rst_n;

// Host TileLink interface connections
`CONNECT_TL_HOST_IF(dbg, dut, clk_dbg_i, rst_n)

// Device TileLink interface connections
`CONNECT_TL_DEVICE_IF(mbx_jtag__soc, dut, clk_dbg_i, rst_n)