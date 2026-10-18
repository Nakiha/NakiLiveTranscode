#!/usr/bin/env python3
"""Audit arithmetic, causality and independent cap policies."""
import importlib.util
import pathlib
import unittest
ROOT=pathlib.Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('audit',ROOT/'scripts/audit-live-budget.py')
audit=importlib.util.module_from_spec(spec);spec.loader.exec_module(audit)

class BudgetAudit(unittest.TestCase):
    def test_rolling_half_open_and_real_bursts(self):
        result=audit.rolling([(0,600),(999999,500),(1000000,400)],1000)
        self.assertEqual([r['bits_in_last_second'] for r in result['samples']],[600,1100,900])
        self.assertEqual(result['violation_events'],1)

    def test_source_budget_uses_only_arrived_bytes_and_does_not_forgive_debt(self):
        result=audit.buckets([(500000,1000)],[(0,100),(600000,100),(700000,100)],1000,1000)
        self.assertEqual(result['source_violation_events'],3)
        self.assertEqual([round(r['source_credit_bits']) for r in result['rows']],[-100,-100,-100])

    def test_absolute_and_source_policies_can_bind_independently(self):
        low=audit.buckets([(0,100)],[(500000,300)],1000,1000)
        high=audit.buckets([(0,10000)],[(500000,800)],1000,1000)
        self.assertEqual((low['source_violation_events'],low['absolute_violation_events']),(1,0))
        self.assertEqual((high['source_violation_events'],high['absolute_violation_events']),(0,1))

    def test_declared_absolute_startup_window_never_credits_unread_source(self):
        result=audit.buckets([(100000,1000)],[(0,300)],1000,1000,initial_absolute=1000,observed_source_startup=True)
        self.assertEqual(result['source_violation_events'],1)
        self.assertEqual(result['absolute_violation_events'],0)

    def test_source_startup_is_limited_to_observed_window(self):
        result=audit.buckets([(0,100)],[(100000,80)],1000,1000,initial_absolute=1000,observed_source_startup=True)
        self.assertEqual(result['initial_source_credit_bits'],100)
        self.assertEqual(result['source_violation_events'],0)

    def test_credit_is_bounded_and_expiry_stops_source_income(self):
        result=audit.buckets([(0,10000)],[(2000000,600)],10000,500)
        self.assertEqual(result['rows'][0]['source_rate_bps'],0)
        self.assertEqual(result['rows'][0]['source_credit_bits'],-100)
        self.assertEqual(result['rows'][0]['absolute_credit_bits'],-100)

if __name__=='__main__':unittest.main()
