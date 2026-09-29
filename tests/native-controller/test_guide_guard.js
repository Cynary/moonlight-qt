const assert = require('node:assert/strict');
const vm = require('node:vm');
const fs = require('node:fs');
const path = require('node:path');
let now=0, timer, calls=[];
class Source { OnSystemButtonPress(button,index) { calls.push([button,index]); } }
const source=new Source();
const context={window:{},performance:{now:()=>now},setInterval:f=>(timer=f,1),clearInterval:()=>{},
  FocusNavController:{m_rgGamepadInputSources:[source]},ControllerStore:{
    GetController:index=>({nControllerIndex:index,eControllerType:index===1?10:31}),
    GetControllerTypeString:type=>type===10?'controller_steamcontroller_triton':'controller_xbox360',
    GetControllers:()=>[{nControllerIndex:1,eControllerType:10}]}};
const script=fs.readFileSync(path.join(__dirname,'../../app/deploy/linux/native-controller/guide_guard.js'),'utf8');
vm.runInNewContext(script,context);
source.OnSystemButtonPress(27,1);assert.equal(calls.length,0);
source.OnSystemButtonPress(27,2);assert.deepEqual(calls,[[27,2]]);
source.OnSystemButtonPress(14,1);assert.deepEqual(calls[1],[14,1]);
assert.equal(context.window.__moonmachineGuideGuard.local(),true);assert.deepEqual(calls[2],[27,1]);
vm.runInNewContext(script,context);assert.equal(context.window.__moonmachineGuideGuard.intercepted,1);
now=4000;timer();assert.equal(context.window.__moonmachineGuideGuard,undefined);
assert.equal(Object.hasOwn(source,'OnSystemButtonPress'),false);
source.OnSystemButtonPress(27,1);assert.equal(calls.length,4);
console.log('PASS: single-source suppression, other controllers/buttons, local replay, idempotency, lease recovery');
