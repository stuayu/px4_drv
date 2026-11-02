/* Use a per-file category for user-space builds of the shared chip drivers. */
#pragma once

#ifdef __KERNEL__
/* Linux keeps native dev_* macros; dynamic debug controls dev_dbg output. */
#else
#ifndef PX4_LOG_DEFAULT_CATEGORY
#define PX4_LOG_DEFAULT_CATEGORY PX4_LOG_CAT_DEVICE
#endif

#undef dev_err
#undef dev_warn
#undef dev_info
#undef dev_dbg

#define dev_err(dev, format, ...) \
	px4_log_err((dev), PX4_LOG_DEFAULT_CATEGORY, format, ##__VA_ARGS__)
#define dev_warn(dev, format, ...) \
	px4_log_warn((dev), PX4_LOG_DEFAULT_CATEGORY, format, ##__VA_ARGS__)
#define dev_info(dev, format, ...) \
	px4_log_info((dev), PX4_LOG_DEFAULT_CATEGORY, format, ##__VA_ARGS__)
#define dev_dbg(dev, format, ...) \
	px4_log_debug((dev), PX4_LOG_DEFAULT_CATEGORY, format, ##__VA_ARGS__)
#endif
