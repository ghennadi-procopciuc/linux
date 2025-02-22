/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright 2022 NXP
 */
#ifndef __DT_BINDINGS_SCMI_PERF_S32CC_H
#define __DT_BINDINGS_SCMI_PERF_S32CC_H

#define S32CC_SCMI_PERF_BASE_ID		0U
#define S32CC_SCMI_PERF(N)		((N) + S32CC_SCMI_PERF_BASE_ID)

#define S32CC_SCMI_PERF_A53		S32CC_SCMI_PERF(0)

#define S32CC_SCMI_PERF_MAX_ID		S32CC_SCMI_PERF(1)

#endif
