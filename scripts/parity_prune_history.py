
import os, copy, sys
os.environ.pop('KIMIX_NATIVE_SOUL', None)
import kimix_native.soul as soul

def _msg(role, content, tool_calls=None, tool_call_id=None):
    m = {'role': role, 'content': content}
    if tool_calls is not None: m['tool_calls'] = tool_calls
    if tool_call_id is not None: m['tool_call_id'] = tool_call_id
    return m

cases = []
cases.append(('empty', [], {'stable_prefix_messages':0,'recent_messages_protected':0,'max_elision_tokens':1000}))
cases.append(('all_protected', [_msg('user','u'), _msg('assistant','a')], {'stable_prefix_messages':2,'recent_messages_protected':2,'max_elision_tokens':1000}))
cases.append(('sys_reminder', [_msg('user','<system-reminder>r</system-reminder>'), _msg('assistant','ok')], {'stable_prefix_messages':0,'recent_messages_protected':1,'max_elision_tokens':1000}))
cases.append(('notification', [_msg('user','<notification x=1>'), _msg('assistant','ok')], {'stable_prefix_messages':0,'recent_messages_protected':1,'drop_notifications':True,'max_elision_tokens':1000}))
cases.append(('task_snapshot', [_msg('user','<active-background-tasks>A</active-background-tasks>'), _msg('user','<active-background-tasks>B</active-background-tasks>'), _msg('assistant','ok')], {'stable_prefix_messages':0,'recent_messages_protected':1,'drop_task_snapshots':True,'max_elision_tokens':1000}))
cases.append(('dmail', [_msg('user','D-Mail from your future self'), _msg('assistant','ok')], {'stable_prefix_messages':0,'recent_messages_protected':1,'drop_dmail':True,'max_elision_tokens':1000}))
cases.append(('checkpoint', [_msg('user','<system>CHECKPOINT</system>'), _msg('assistant','ok')], {'stable_prefix_messages':0,'recent_messages_protected':1,'drop_checkpoints':True,'max_elision_tokens':1000}))
cases.append(('tool_pair', [_msg('assistant',[],[{'type':'function','id':'tc1','function':{'name':'f','arguments':'{}'}}]), _msg('tool',[{'type':'text','text':'result'}], tool_call_id='tc1'), _msg('assistant','ok')], {'stable_prefix_messages':0,'recent_messages_protected':1,'max_elision_tokens':1000}))
text = 'x' * (512*4 - 4)
cases.append(('oversized', [_msg('user','q'), _msg('tool',[{'type':'text','text':text}], tool_call_id='tc1'), _msg('assistant','a')], {'stable_prefix_messages':0,'recent_messages_protected':1,'max_elision_tokens':10000,'oversized_output_enabled':True,'superseded_read_enabled':False,'stale_tool_result_enabled':False}))
cases.append(('superseded', [_msg('user','q'), _msg('tool',[{'type':'text','text':'long long long long long long long long long long'}], tool_call_id='tc1'), _msg('tool',[{'type':'text','text':'short'}], tool_call_id='tc2'), _msg('assistant','ok')], {'stable_prefix_messages':0,'recent_messages_protected':1,'max_elision_tokens':10000,'superseded_read_enabled':True,'oversized_output_enabled':False,'stale_tool_result_enabled':False}))
cases.append(('resolved', [_msg('user','q'), _msg('tool',[{'type':'text','text':'<system>ERROR: boom</system>'}], tool_call_id='tc1'), _msg('tool',[{'type':'text','text':'fixed'}], tool_call_id='tc2'), _msg('assistant','ok')], {'stable_prefix_messages':0,'recent_messages_protected':1,'max_elision_tokens':10000,'stale_tool_result_enabled':True,'superseded_read_enabled':False,'oversized_output_enabled':False}))
cases.append(('negation', [_msg('user','<notification x=1>'), _msg('assistant','ok')], {'stable_prefix_messages':0,'recent_messages_protected':1,'drop_notifications':False,'max_elision_tokens':1000}))
cases.append(('string_content', [_msg('user','<system-reminder>r</system-reminder>'), _msg('assistant','ok')], {'stable_prefix_messages':0,'recent_messages_protected':1,'max_elision_tokens':1000}))
cases.append(('current_turn', [_msg('user','a'), _msg('user','b'), _msg('assistant','c')], {'stable_prefix_messages':0,'recent_messages_protected':0,'current_turn_index':1,'max_elision_tokens':1000}))

ok = True
for name, msgs, policy in cases:
    nat = soul.prune_history(copy.deepcopy(msgs), copy.deepcopy(policy))
    os.environ['KIMIX_NATIVE_SOUL'] = '0'
    fb = soul._compat_prune_history(copy.deepcopy(msgs), copy.deepcopy(policy))
    os.environ.pop('KIMIX_NATIVE_SOUL', None)
    if nat != fb:
        print(f'FAIL {name}')
        print('nat:', nat)
        print('fb:', fb)
        ok = False
    else:
        print(f'PASS {name}')

sys.exit(0 if ok else 1)
