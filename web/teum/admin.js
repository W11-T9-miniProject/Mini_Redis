/* 편집 흐름: JSON 변경 요청 → SQLite 커밋 → 관련 접두어 DEL → 결과 표시.
 * 동작 상태는 서버 응답으로만 표시한다. 삭제가 실패하면 '반영 대기'를 숨기지 않는다. */
const $ = selector => document.querySelector(selector);
let token = '', page = 1, query = '', editingId = null, deleting = null, rows = [];
let listVersion = 0;
let busy = false;

async function request(path, {method='GET', body}={}) {
  const response = await fetch(path, {method, headers:method==='GET' ? {} : {'Content-Type':'application/json','X-Admin-Token':token}, body:body===undefined ? undefined : JSON.stringify(body)});
  const data = await response.json();
  if (!response.ok) throw new Error(data.error || '요청을 처리하지 못했어요.');
  return data;
}
function resetEditor() {
  editingId = null;
  $('#phrase-form').reset();
  $('#phrase-kind').value = '키워드';
  $('#editor-heading').textContent = '새 검색어 추가';
  $('#editor-description').textContent = '장소 정보는 유지하고 검색창의 추천어를 관리합니다.';
  $('#save-phrase').textContent = '검색어 추가하기 ＋';
  $('#cancel-edit').hidden = true;
  $('#editor-error').hidden = true;
  markEditingRow();
}
function markEditingRow() {
  for (const row of document.querySelectorAll('#catalog-rows tr')) row.classList.toggle('is-editing',Number(row.dataset.id)===editingId);
}
function beginEdit(phrase) {
  if(busy)return;
  editingId=phrase.id;
  $('#phrase-text').value=phrase.text;
  $('#phrase-kind').value=phrase.kind;
  $('#editor-heading').textContent='검색어 수정';
  $('#editor-description').textContent='입력한 이름과 분류로 추천 검색어를 바꿉니다.';
  $('#save-phrase').textContent='변경 사항 저장 ↗';
  $('#cancel-edit').hidden=false;
  $('#editor-error').hidden=true;
  markEditingRow();
  $('.editor-panel').scrollIntoView({behavior:'smooth',block:'nearest'});
  $('#phrase-text').focus();
}
function beginDelete(phrase) {
  if(busy)return;
  deleting=phrase;
  $('#delete-name').textContent=phrase.text;
  $('#delete-error').hidden=true;
  $('#delete-dialog').showModal();
}
function showReceipt(action, phrase, invalidation) {
  $('#change-receipt').hidden=false;
  const verb={create:'추가했습니다',update:'수정했습니다',delete:'삭제했습니다'}[action];
  $('#receipt-title').textContent=`‘${phrase}’ 검색어를 ${verb}.`;
  $('#receipt-description').textContent=invalidation.pending
    ? `변경 내용은 저장됐습니다. 이전 추천 ${invalidation.pending}건의 정리를 다음 검색에서 다시 시도합니다.`
    : `관련 검색어 ${invalidation.affected}개를 갱신하고 이전 추천 ${invalidation.deleted}건을 정리했습니다. 다음 검색부터 반영됩니다.`;
  $('#receipt-time').textContent=new Date().toLocaleTimeString('ko-KR',{hour:'2-digit',minute:'2-digit'});
}
async function loadList() {
  const version=++listVersion;
  $('#catalog-status').textContent='목록을 불러오고 있어요.';
  try {
    const data=await request('/api/admin/phrases?'+new URLSearchParams({q:query,page}));
    if(version!==listVersion)return;
    if(page>1 && !data.phrases.length){page=Math.max(1,Math.ceil(data.total/data.page_size));return loadList();}
    rows=data.phrases;
    $('#admin-total').textContent=data.total;
    $('#catalog-status').textContent=data.pending ? `반영 대기 ${data.pending}건 · 다음 검색에서 다시 시도합니다.` : query ? `‘${query}’와 일치하는 검색어입니다.` : '검색어를 선택해 수정하거나 새 검색어를 추가하세요.';
    const body=$('#catalog-rows');body.replaceChildren();
    for(const phrase of rows){
      const row=document.createElement('tr');row.dataset.id=phrase.id;
      const name=document.createElement('td');name.textContent=phrase.text;
      const kind=document.createElement('td');const pill=document.createElement('span');pill.className='kind-pill';pill.textContent=phrase.kind;kind.append(pill);
      const actions=document.createElement('td');
      const edit=document.createElement('button');edit.type='button';edit.className='row-edit';edit.textContent='수정';edit.setAttribute('aria-label',phrase.text+' 수정');edit.addEventListener('click',()=>beginEdit(phrase));
      const remove=document.createElement('button');remove.type='button';remove.className='row-delete';remove.textContent='삭제';remove.setAttribute('aria-label',phrase.text+' 삭제');remove.addEventListener('click',()=>beginDelete(phrase));
      actions.append(edit,remove);row.append(name,kind,actions);body.append(row);
    }
    if(!rows.length){const row=document.createElement('tr');const cell=document.createElement('td');cell.colSpan=3;cell.className='catalog-empty';cell.textContent='일치하는 검색어가 없어요.';row.append(cell);body.append(row);}
    $('#catalog-page').textContent=`${page} / ${Math.max(1,Math.ceil(data.total/data.page_size))}`;
    $('#previous-page').disabled=page<=1;$('#next-page').disabled=page*data.page_size>=data.total;
    markEditingRow();
  } catch(error) {if(version===listVersion)$('#catalog-status').textContent=error.message;}
}
$('#phrase-form').addEventListener('submit',async event=>{
  event.preventDefault();
  if(busy)return;
  busy=true;
  const action=editingId===null?'create':'update';
  const text=$('#phrase-text').value.trim(),kind=$('#phrase-kind').value.trim();
  $('#save-phrase').disabled=true;$('#editor-error').hidden=true;
  try {
    const result=await request('/api/admin/phrases'+(editingId===null?'':'/'+editingId),{method:editingId===null?'POST':'PUT',body:{text,kind}});
    showReceipt(action,text,result.invalidation);resetEditor();await loadList();
  }catch(error){$('#editor-error').textContent=error.message;$('#editor-error').hidden=false;}
  finally{busy=false;$('#save-phrase').disabled=false;}
});
$('#confirm-delete').addEventListener('click',async()=>{
  if(!deleting||busy)return;
  busy=true;
  $('#confirm-delete').disabled=true;$('#delete-error').hidden=true;
  try {
    const result=await request('/api/admin/phrases/'+deleting.id,{method:'DELETE',body:{}});
    if(editingId===deleting.id)resetEditor();
    showReceipt('delete',deleting.text,result.invalidation);$('#delete-dialog').close();await loadList();
  }catch(error){$('#delete-error').textContent=error.message;$('#delete-error').hidden=false;}
  finally{busy=false;$('#confirm-delete').disabled=false;}
});
$('#admin-filter').addEventListener('submit',event=>{event.preventDefault();query=$('#admin-filter-input').value.trim();page=1;loadList();});
$('#cancel-edit').addEventListener('click',()=>{if(!busy)resetEditor();});
$('#reload-catalog').addEventListener('click',loadList);
$('#previous-page').addEventListener('click',()=>{page=Math.max(1,page-1);loadList();});
$('#next-page').addEventListener('click',()=>{page+=1;loadList();});
async function init(){
  $('#save-phrase').disabled=true;
  try{token=(await request('/api/admin/session')).token;$('#save-phrase').disabled=false;await loadList();}
  catch(error){$('#catalog-status').textContent=error.message;}
}
init();
