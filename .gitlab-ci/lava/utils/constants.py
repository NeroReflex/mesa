from os import getenv

# How many attempts should be made when a timeout happen during LAVA device boot.
NUMBER_OF_ATTEMPTS_LAVA_BOOT = int(getenv("LAVA_NUMBER_OF_ATTEMPTS_LAVA_BOOT", 3))


# Supports any integers in [0, 100].
# The scheduler considers the job priority when ordering the queue
# to consider which job should run next.
JOB_PRIORITY = int(getenv("JOB_PRIORITY", 75))

# Use UART over the default SSH mechanism to follow logs.
# Caution: this can lead to device silence in some devices in Mesa CI.
FORCE_UART = bool(getenv("LAVA_FORCE_UART", False))

# How many times the r8152 error may happen to consider it a known issue.
KNOWN_ISSUE_R8152_MAX_CONSECUTIVE_COUNTER: int = 10

# This is considered noise, since LAVA produces this log after receiving a package of feedback
# messages.
LOG_DEBUG_FEEDBACK_NOISE = "Listened to connection for namespace 'dut' done"
