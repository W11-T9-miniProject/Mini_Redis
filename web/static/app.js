'use strict';
const $ = id => document.getElementById(id);
const adminPage = document.body.dataset.admin === 'true';
let state = null, online = false, busy = false, sequence = 0, skew = 0, editing = false;
let qrUrl = null;
const buttons = new Map();
const labels = {available:'빈자리',held:'임시 선점',confirmed:'배정 완료',vacant:'공석'};
const timeText = ms => new Intl.DateTimeFormat('ko-KR',{timeZone:'Asia/Seoul',month:'numeric',day:'numeric',hour:'2-digit',minute:'2-digit',hour12:false}).format(new Date(ms));
function notice(message, error=false) { $('notice').textContent=message; $('notice').className='notice'+(error?' error':''); $('notice').hidden=false; }
async function api(path, data) {
  const response=await fetch(path, data===undefined?{}:{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(data)});
  const result=await response.json();
  if(!response.ok) throw new Error(result.error||'요청을 완료하지 못했습니다.');
  return result;
}
async function action(path, data, message) {
  if(busy) return;
  busy=true; render();
  let succeeded=false;
  try { await api(path,data); succeeded=true;if(message) notice(message); }
  catch(error) { notice(error.message||'처리 결과를 확인할 수 없습니다.',true); }
  finally { busy=false; await refresh(); }
  return succeeded;
}
function element(tag, text, className) { const node=document.createElement(tag); if(text!==undefined)node.textContent=text; if(className)node.className=className; return node; }
function mySeat() { return state?.seats.find(s=>s.is_mine); }
function claim(seat) { action('/api/claim',{seat_id:seat.id},'20초 안에 자리를 확정해 주세요.'); }
function render() {
  $('connection').textContent=online?'상태 연결됨':'연결 확인 중'; $('connection').classList.toggle('online',online);
  if(!state) return;
  const mine=mySeat(), participant=state.participant;
  const available=state.seats.filter(s=>s.status==='available');
  $('available-count').textContent=available.length; $('available-label').textContent=available.length+'석';
  $('held-count').textContent=state.seats.filter(s=>s.status==='held').length;
  $('join-form').hidden=!!participant&&!editing; $('profile').hidden=!participant||editing;
  if(participant) { $('profile-name').textContent=participant.full_name; $('profile-number').textContent=participant.student_no+'번 교육생'; $('edit-profile').disabled=!!mine; }
  for(const seat of state.seats) {
    let button=buttons.get(seat.id);
    if(!button) {
      button=element('button'); button.type='button';
      Object.assign(button.style,{left:(seat.x/1760*100)+'%',top:(seat.y/1040*100)+'%',width:(seat.w/1760*100)+'%',height:(seat.h/1040*100)+'%'});
      button.addEventListener('click',()=>{
        const current=state.seats.find(s=>s.id===seat.id);
        if(!current.selectable)return;
        if(adminPage&&state.admin&&current.status!=='available') { $('admin-seat').value=seat.id; notice(current.label+' 배정 취소 대상으로 선택했습니다.'); }
        else claim(current);
      });
      $('room').append(button); buttons.set(seat.id,button);
    }
    button.className='seat '+seat.status+(seat.is_mine?' is-mine':'')+(seat.demo?' special':'');
    const canAdmin=adminPage&&state.admin&&seat.selectable&&seat.status!=='available';
    button.disabled=!seat.selectable||busy||!online||(!canAdmin&&(!participant||editing||!!mine||seat.status!=='available'));
    const seatName=element('b');
    if(seat.selectable)seatName.append(element('span',seat.team+'팀'),element('span','-'+seat.position));
    else seatName.textContent='공석';
    const mapStatus=seat.status==='confirmed'?'배정':seat.status==='held'?'선점':'빈자리';
    button.replaceChildren(seatName,element('span',seat.selectable?mapStatus:'','map-state'));
    button.setAttribute('aria-label',seat.label+' '+labels[seat.status]+' '+seat.full_name);
    button.title=seat.label+' · '+labels[seat.status]+(seat.full_name?' · '+seat.full_name:'');
  }
  $('choices').replaceChildren();
  if(!available.length) $('choices').append(element('p','지금은 선택 가능한 자리가 없습니다.','subtle'));
  for(const seat of available) {
    const button=element('button',undefined,'choice'); button.type='button';
    button.append(element('span',seat.label),element('small','선점 →'));
    button.disabled=!participant||editing||!!mine||busy||!online; button.onclick=()=>claim(seat); $('choices').append(button);
  }
  const minePanel=$('my-seat'); minePanel.replaceChildren();
  if(!mine) minePanel.append(element('p',participant?'아직 선택한 자리가 없습니다. 빈자리를 골라 주세요.':'성명과 번호를 입력하고 빈자리를 선택해 주세요.','subtle'));
  else {
    minePanel.append(element('h3',mine.label));
    if(mine.status==='held') { minePanel.append(element('p','확정까지 남은 시간','subtle'),element('div',undefined,'countdown')); }
    else minePanel.append(element('p','배정이 확정되었습니다.','subtle'),element('p',timeText(mine.expires_at_ms)+'까지 유효','subtle'));
    const actions=element('div',undefined,'my-actions');
    if(mine.status==='held') {
      const confirm=element('button','자리 확정'); confirm.id='confirm-seat'; confirm.disabled=busy||!online;
      confirm.onclick=()=>action('/api/confirm',{seat_id:mine.id,reservation_id:mine.reservation_id},'자리 배정을 확정했습니다.'); actions.append(confirm);
    }
    const cancel=element('button',mine.status==='held'?'선점 취소':'배정 취소','secondary'); cancel.disabled=busy||!online;
    cancel.onclick=()=>action('/api/cancel',{seat_id:mine.id,reservation_id:mine.reservation_id,expected_state:mine.status},'자리를 취소했습니다.'); actions.append(cancel); minePanel.append(actions);
  }
  if(adminPage) {
    $('admin-login').hidden=state.admin; $('admin-controls').hidden=!state.admin;
    if(state.admin) {
      const select=$('admin-seat'), previous=select.value; select.replaceChildren();
      for(const seat of state.seats.filter(s=>s.selectable&&s.status!=='available')) { const option=element('option',seat.label+' · '+seat.full_name);option.value=seat.id;select.append(option); }
      if([...select.options].some(o=>o.value===previous)) select.value=previous;
      const restore=$('restore-seat'), restorePrevious=restore.value;
      const restorable=available.filter(s=>!s.demo);
      restore.replaceChildren();
      for(const seat of restorable){const option=element('option',seat.label);option.value=seat.id;restore.append(option);}
      if(!restorable.length){const empty=element('option','복구할 빈자리가 없습니다.');empty.value='';restore.append(empty);}
      if([...restore.options].some(o=>o.value===restorePrevious))restore.value=restorePrevious;
      restore.disabled=!restorable.length||busy;
      $('admin-restore').disabled=!restorable.length||busy||!online;
      const release=$('release-number'), releasePrevious=release.value;
      const registrations=state.registrations||[];
      release.replaceChildren();
      for(const person of registrations){
        const status=person.seat_label?person.seat_label+' '+labels[person.seat_status]+' · 자리 취소 필요':'자리 없음 · 초기화 가능';
        const option=element('option',person.student_no+'번 · '+person.full_name+' · '+status);
        option.value=String(person.student_no);option.disabled=!!person.seat_label;release.append(option);
      }
      if(!registrations.length){const empty=element('option','등록된 교육생이 없습니다.');empty.value='';release.append(empty);}
      if([...release.options].some(o=>o.value===releasePrevious&&!o.disabled))release.value=releasePrevious;
      const releasable=registrations.some(p=>!p.seat_label);
      release.disabled=busy||!registrations.length;
      $('admin-release').disabled=busy||!online||!releasable;
      if(document.activeElement!==$('reset-at')&&!$('reset-at').value) setDate(state.settings.reset_at_ms);
      if(document.activeElement!==$('public-url')&&!$('public-url').dataset.edited) $('public-url').value=state.settings.public_url||'';
    }
    const url=state.settings.public_url;
    if(url&&url!==qrUrl) { qrUrl=url; const img=element('img');img.src='/qr.svg?v='+encodeURIComponent(url);img.alt='자리 선점 참여 QR';const link=element('a',url);link.href=url;link.target='_blank';link.rel='noopener';$('qr-content').replaceChildren(img,link); }
    else if(!url&&qrUrl) {qrUrl=null;$('qr-content').replaceChildren(element('p','외부 접속 주소를 설정하면 QR이 표시됩니다.','subtle'));}
  }
  tick();
}
function tick() {
  const mine=mySeat(), countdown=document.querySelector('.countdown');
  if(!mine||mine.status!=='held'||!countdown) return;
  const remaining=Math.max(0,Math.ceil((mine.expires_at_ms-Date.now()-skew)/1000));
  countdown.replaceChildren(document.createTextNode(String(remaining)),element('small','초'));
  if($('confirm-seat')) $('confirm-seat').disabled=remaining===0||busy||!online;
}
async function refresh() {
  const id=++sequence;
  try { const next=await api('/api/state'); if(id!==sequence)return; const previous=mySeat();if(previous?.status==='held'&&!next.seats.some(s=>s.is_mine)&&next.server_now_ms>=previous.expires_at_ms)notice('임시 선점 20초가 지나 자리가 다시 열렸습니다.');state=next;skew=state.server_now_ms-Date.now();online=true; }
  catch(error) { if(id!==sequence)return;online=false;notice(error.message||'연결을 확인 중입니다.',true); }
  render();
}
function setDate(ms) { const d=new Date(ms); $('reset-at').value=new Date(d.getTime()-d.getTimezoneOffset()*60000).toISOString().slice(0,19); }
$('join-form').onsubmit=async event=>{event.preventDefault();const ok=await action('/api/join',{full_name:$('full-name').value,student_no:Number($('student-no').value)},'참여 정보가 등록되었습니다.');if(ok&&state?.participant){editing=false;render();}};
$('edit-profile').onclick=async()=>{const previous=state.participant;const ok=await action('/api/leave',{},'기존 등록을 종료했습니다. 새 참여 정보를 입력해 주세요.');if(ok){editing=true;$('full-name').value=previous.full_name;$('student-no').value=previous.student_no;render();$('full-name').focus();}};
if(adminPage) {
  $('admin-login-form').onsubmit=async event=>{event.preventDefault();await action('/api/admin/login',{key:$('admin-key').value},'관리자 인증이 완료되었습니다.');$('admin-key').value='';};
  $('one-minute').onclick=()=>setDate(Date.now()+skew+60000);
  $('public-url').oninput=()=>{$('public-url').dataset.edited='true';};
  $('settings-form').onsubmit=async event=>{event.preventDefault();const data={reset_at_ms:new Date($('reset-at').value).getTime(),public_url:$('public-url').value.trim()};await action('/api/admin/settings',data,'이후 확정에 적용할 시연 설정을 저장했습니다.');delete $('public-url').dataset.edited;};
  $('admin-cancel-form').onsubmit=event=>{event.preventDefault();if($('admin-seat').value)action('/api/admin/cancel',{seat_id:$('admin-seat').value},'선택한 자리의 배정을 취소했습니다.');};
  $('admin-restore-form').onsubmit=event=>{event.preventDefault();if($('restore-seat').value)action('/api/admin/restore',{seat_id:$('restore-seat').value},'기존 배정으로 되돌렸습니다.');};
  $('admin-release-form').onsubmit=event=>{event.preventDefault();if($('release-number').value)action('/api/admin/release',{student_no:Number($('release-number').value)},'번호 등록을 초기화했습니다. 새 기기에서 다시 참여할 수 있습니다.');};
}
refresh();setInterval(()=>{if(!busy)refresh();},2000);setInterval(tick,200);
