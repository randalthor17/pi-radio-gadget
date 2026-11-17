# SPDX-License-Identifier: GPL-2.0-or-later
tests = ['ffs', 'pktq', 'acl_split', 'bdc', 'fw', 'wpa_parse', 'tap', 'epwriter', 'sco', 'update']
foreach t : tests
  test(t, executable('test_' + t, 'test_' + t + '.c', dependencies : core_dep))
endforeach
