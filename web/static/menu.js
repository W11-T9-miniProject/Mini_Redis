'use strict';
const menuElement=id=>document.getElementById(id);
let menuBusy=false, cacheDeadline=null, cacheRemainingMs=0, cacheObservedAt=0;
function menuNotice(message,error=false){const e=menuElement('menu-notice');e.textContent=message;e.className='menu-feedback'+(error?' error':'');}
function localTime(ms){return new Date(ms).toLocaleString('ko-KR',{timeZone:'Asia/Seoul',hour12:false});}
function expiryTime(ms){return new Date(ms).toLocaleTimeString('ko-KR',{timeZone:'Asia/Seoul',hour12:false});}
function tickCache(){
 if(cacheDeadline===null)return;
 const seconds=Math.max(0,Math.ceil((cacheRemainingMs-(performance.now()-cacheObservedAt))/1000));
 menuElement('cache-remaining').textContent=seconds?Math.floor(seconds/60)+'분 '+String(seconds%60).padStart(2,'0')+'초':'기한 만료';
 if(!seconds)menuElement('menu-deadline').textContent='다음 조회에서 원본을 다시 가져옵니다.';
}
async function menuRequest(path,post=false){const response=await fetch(path,post?{method:'POST',headers:{'Content-Type':'application/json'},body:'{}'}:{});const result=await response.json();if(!response.ok)throw new Error(result.error||'요청을 완료하지 못했습니다.');return result;}
function showMenu(data){
 menuElement('cache-badge').textContent=data.cache==='HIT'?'HIT · 캐시에서 조회':data.cache==='MISS'?'MISS · 원본 조회 후 저장':'BYPASS · 원본 직접 조회';
 menuElement('menu-title').textContent=data.menu.title;
 menuElement('menu-elapsed').textContent=data.elapsed_ms.toFixed(3);
 menuElement('menu-fetched').textContent='마지막 원본 조회 '+localTime(data.fetched_at_ms);
 if(data.expires_at_ms){cacheDeadline=data.expires_at_ms;cacheRemainingMs=cacheDeadline-data.server_now_ms;cacheObservedAt=performance.now();menuElement('menu-deadline').textContent=expiryTime(cacheDeadline)+' 만료 · 조회 기준';tickCache();}
 const img=menuElement('menu-image');if(img.getAttribute('src')!==data.menu.image_url)img.src=data.menu.image_url;
 menuElement('menu-image-link').href=data.menu.image_url;menuElement('menu-image-link').hidden=false;menuElement('menu-source').href=data.menu.source_url;
}
async function menuAction(work){if(menuBusy)return;menuBusy=true;document.querySelectorAll('main button').forEach(b=>b.disabled=true);try{await work();}catch(error){menuNotice(error.message||'연결을 확인해 주세요.',true);}finally{menuBusy=false;document.querySelectorAll('main button').forEach(b=>b.disabled=false);}}
menuElement('menu-refresh').onclick=()=>menuAction(async()=>{menuNotice('식단 조회 중…');showMenu(await menuRequest('/api/menu'));menuNotice('');});
menuElement('menu-image').onerror=()=>menuNotice('식단 이미지를 불러오지 못했습니다. 카카오 원본 링크를 확인해 주세요.',true);
function resultElement(tag,text,className){const e=document.createElement(tag);if(text!==undefined)e.textContent=text;if(className)e.className=className;return e;}
if(menuElement('menu-clear')){
 menuElement('menu-clear').onclick=()=>menuAction(async()=>{
  await menuRequest('/api/admin/menu/clear',true);cacheDeadline=null;menuElement('cache-badge').textContent='캐시 비움';menuElement('cache-remaining').textContent='캐시 없음';menuElement('menu-elapsed').textContent='—';menuElement('menu-deadline').textContent='다음 조회에서 원본을 다시 가져옵니다.';
  menuElement('benchmark-results').replaceChildren();menuNotice('캐시 비움 · 다시 조회해 주세요.');
 });
 menuElement('menu-benchmark').onclick=()=>menuAction(async()=>{
  menuNotice('성능 비교 중…');const result=await menuRequest('/api/admin/menu/benchmark',true);
  const target=menuElement('benchmark-results');target.replaceChildren();const cards=resultElement('div',undefined,'benchmark-cards');
  for(const [label,s] of [['캐시 없음 · 원본',result.uncached],['캐시 있음 · Mini Redis',result.cached]]){
   const card=resultElement('article',undefined,'benchmark-card');card.append(resultElement('h2',label));const metric=resultElement('p',undefined,'menu-metric');metric.append(resultElement('strong',s.mean_ms.toFixed(3)),resultElement('span','ms'));card.append(metric,resultElement('p','5회 평균','subtle'));cards.append(card);
  }target.append(cards);
  const table=document.createElement('table');const head=document.createElement('tr');for(const title of ['조회 방식','중앙값 (ms)','최소–최대 (ms)'])head.append(resultElement('th',title));table.append(head);
  for(const [label,s] of [['캐시 없음',result.uncached],['캐시 있음',result.cached]]){const row=document.createElement('tr');for(const value of [label,s.median_ms.toFixed(3),s.min_ms.toFixed(3)+'–'+s.max_ms.toFixed(3)])row.append(resultElement('td',value));table.append(row);}target.append(table);
  target.append(resultElement('p','각 5회 · 응답 '+(result.same_payload?'내용 동일':'내용 변경 감지')+' · 서버 내부 처리 시간','subtle'));
  showMenu(await menuRequest('/api/menu'));if(result.same_payload)menuNotice('');else menuNotice('원본 변경 감지 · 같은 내용의 비교가 아닙니다.',true);
 });
}
setInterval(tickCache,1000);
menuElement('menu-refresh').click();
