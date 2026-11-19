# SPDX-License-Identifier: GPL-3.0-or-later
"""Field and state names of the native modem status (debug-link command 0x21, sub-operation 0): what the read-only
readers (plus_txdiag.py, plus_live.py) print. The lab's bench interface (transmit leases, receive captures) is not part
of the public program; nothing here sends anything."""
FIELDS = ('version state reason session owned mode frequency_hz blocks duration_ms lease_ms rx_count tx_count rx_seen '
          'rx_saved rx_dropped rx_read tx_loaded tx_sent phy_irqs tx_slot_irqs rx_slot_irqs rf_irqs abnormal_irqs '
          'duplicate_irqs ignored_irqs pa_keys quiets faults rejected max_gap_us last_gap_us restore_failures '
          'started_us boundary_us p25_sync key_requested keyed').split()
STATES = ('idle', 'armed', 'receive', 'transmit', 'draining', 'stopped', 'fault')
REASONS = ('ok', 'user_stop', 'rx_limit', 'tx_complete', 'rx_duration', 'lease_expired', 'deadline', 'stalled',
           'late_boundary', 'early_boundary', 'underrun', 'io', 'rx_overflow', 'abnormal_slot', 'denied',
           'interrupted', 'restore_failed', 'unexpected_rf')
