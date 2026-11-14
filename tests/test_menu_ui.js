'use strict';
const assert=require('node:assert/strict');
const fs=require('node:fs');
const vm=require('node:vm');
let elapsed=0;
const nodes=new Map();
function node(id){if(!nodes.has(id))nodes.set(id,{textContent:'',src:'',className:'',hidden:false,getAttribute(){return this.src;},click(){}});return nodes.get(id);}
class SkewedDate extends Date{static now(){return 9000000000000;}}
const scope=vm.createContext({document:{getElementById:node,querySelectorAll:()=>[]},performance:{now:()=>elapsed},Date:SkewedDate,setInterval:()=>{},fetch:()=>{throw Error('unexpected network');}});
vm.runInContext(fs.readFileSync('web/static/menu.js','utf8'),scope);
vm.runInContext("showMenu({cache:'HIT',menu:{title:'식단표',image_url:'https://k.kakaocdn.net/x',source_url:'https://pf.kakao.com/x'},elapsed_ms:0.5,fetched_at_ms:100000,server_now_ms:100000,expires_at_ms:3700000})",scope);
assert.equal(node('cache-remaining').textContent,'60분 00초');
elapsed=11000;
vm.runInContext('tickCache()',scope);
assert.equal(node('cache-remaining').textContent,'59분 49초');
elapsed=3600001;
vm.runInContext('tickCache()',scope);
assert.equal(node('cache-remaining').textContent,'기한 만료');
console.log('PASS menu countdown ignores wall-clock skew and expires by elapsed time');
vm.runInContext("menuNotice('비교 중…')",scope);
assert.equal(node('menu-notice').className,'menu-feedback');
assert.equal(node('menu-notice').hidden,false);
vm.runInContext("menuNotice('연결 실패',true)",scope);
assert.equal(node('menu-notice').className,'menu-feedback error');
vm.runInContext("menuNotice('')",scope);
assert.equal(node('menu-notice').hidden,false);
assert.equal(node('menu-notice').textContent,'');
console.log('PASS status remains in layout through progress, errors and clearing');
