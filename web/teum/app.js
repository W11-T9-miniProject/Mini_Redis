/* 브라우저는 표시와 사용자 상호작용만 담당한다.
 * 자동완성 후보는 /api/suggest → Python → C 저장소/SQLite 순서로 가져온다.
 * 최근 검색과 저장한 장소는 이 브라우저의 localStorage에만 보관한다. */
const $ = (selector) => document.querySelector(selector);
const input = $('#search-input');
const panel = $('#suggest-panel');
const list = $('#suggestions');
const grid = $('#place-grid');
const dialog = $('#place-dialog');
const BOOKMARK = '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M6 4h12v16l-6-4-6 4z"/></svg>';
const state = {query:'', category:'all', district:'all', sort:'recommended', saved:false, visible:6, places:[]};
let suggestions = [], activeOption = -1, composing = false, suggestTimer;
let suggestionController, resultController, suggestionVersion = 0, resultVersion = 0;
let toastTimer, currentPlace;

function readStorage(key, fallback) {
  try { return JSON.parse(localStorage.getItem(key)) ?? fallback; } catch { return fallback; }
}
let recent = readStorage('teum:recent', []);
if (!Array.isArray(recent)) recent = [];
recent = recent.filter(value => typeof value === 'string' && value.length <= 80).slice(0, 6);
let storedIds = readStorage('teum:saved', []);
if (!Array.isArray(storedIds)) storedIds = [];
const savedIds = new Set(storedIds.filter(Number.isSafeInteger));

function persist(key, value) {
  try { localStorage.setItem(key, JSON.stringify(value)); return true; } catch { return false; }
}
function escapeHtml(value) {
  return String(value).replace(/[&<>"']/g, character => ({'&':'&amp;', '<':'&lt;', '>':'&gt;', '"':'&quot;', "'":'&#39;'}[character]));
}
function toast(message) {
  $('#toast').textContent = message;
  $('#toast').classList.add('is-visible');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => $('#toast').classList.remove('is-visible'), 2400);
}
function remember(query) {
  if (!query) return;
  recent = [query, ...recent.filter(value => value !== query)].slice(0, 6);
  persist('teum:recent', recent);
}
function closeSuggestions() {
  panel.hidden = true;
  input.setAttribute('aria-expanded', 'false');
  input.removeAttribute('aria-activedescendant');
  activeOption = -1;
}
function highlighted(text, query) {
  const index = query ? text.toLocaleLowerCase().indexOf(query.toLocaleLowerCase()) : -1;
  if (index < 0) return escapeHtml(text);
  return escapeHtml(text.slice(0, index)) + '<mark>' + escapeHtml(text.slice(index, index + query.length)) + '</mark>' + escapeHtml(text.slice(index + query.length));
}
function renderSuggestions(values, query) {
  suggestions = values;
  activeOption = -1;
  const isRecent = !query && recent.length > 0;
  $('#suggest-title').textContent = query ? '이런 곳을 찾고 있나요?' : isRecent ? '최근 검색과 새로운 발견' : '가볍게 시작해 보세요';
  $('#clear-recent').hidden = !isRecent;
  list.innerHTML = values.length ? values.map((value, index) => `<li class="suggest-option" id="suggest-${index}" role="option" aria-selected="false" data-index="${index}"><span class="suggest-symbol" aria-hidden="true">${value.recent ? '◷' : '⌕'}</span><span class="suggest-text">${highlighted(value.text, query)}</span><span class="suggest-kind">${escapeHtml(value.recent ? '최근 검색' : value.kind)}</span><span class="suggest-arrow" aria-hidden="true">↖</span></li>`).join('') : '<li class="suggest-empty" role="presentation">일치하는 추천어가 없어요. 입력한 검색어로 찾아볼까요?</li>';
  panel.hidden = false;
  input.setAttribute('aria-expanded', 'true');
  input.removeAttribute('aria-activedescendant');
}
async function fetchSuggestions() {
  const query = input.value.trim();
  const version = ++suggestionVersion;
  suggestionController?.abort();
  suggestionController = new AbortController();
  try {
    const response = await fetch('/api/suggest?' + new URLSearchParams({q:query}), {signal:suggestionController.signal});
    if (!response.ok) throw new Error('suggestions unavailable');
    const data = await response.json();
    if (version !== suggestionVersion || document.activeElement !== input || composing) return;
    let values = data.suggestions;
    if (!query && recent.length) {
      values = [...recent.map(text => ({text, kind:'최근 검색', recent:true})), ...values.filter(item => !recent.includes(item.text))].slice(0, 8);
    }
    renderSuggestions(values, query);
  } catch (error) {
    if (error.name !== 'AbortError' && version === suggestionVersion && document.activeElement === input) {
      renderSuggestions([], query);
      list.innerHTML = '<li class="suggest-empty" role="presentation">추천어를 불러오지 못했어요. Enter를 눌러 직접 검색할 수 있어요.</li>';
    }
  }
}
function scheduleSuggestions() {
  clearTimeout(suggestTimer);
  $('#clear-input').hidden = !input.value;
  // 입력이 바뀐 즉시 버전을 올려 이전 응답이 debounce 중에 도착해도 무시한다.
  ++suggestionVersion;
  suggestionController?.abort();
  closeSuggestions();
  if (!composing) suggestTimer = setTimeout(fetchSuggestions, 90);
}
function selectSuggestion(index) {
  const item = suggestions[index];
  if (!item) return;
  input.value = item.text;
  submitSearch(item.text);
}
function moveOption(direction) {
  if (!suggestions.length) return;
  activeOption = activeOption < 0 ? (direction > 0 ? 0 : suggestions.length - 1)
    : (activeOption + direction + suggestions.length) % suggestions.length;
  for (const [index, option] of [...list.querySelectorAll('[role="option"]')].entries()) {
    option.setAttribute('aria-selected', String(index === activeOption));
  }
  const option = $(`#suggest-${activeOption}`);
  input.setAttribute('aria-activedescendant', option.id);
  option.scrollIntoView({block:'nearest'});
}
function updateControls() {
  $('#saved-count').textContent = savedIds.size;
  $('#nav-saved').classList.toggle('is-active', state.saved);
  $('#nav-saved').setAttribute('aria-pressed', String(state.saved));
  $('#nav-discover').classList.toggle('is-active', !state.saved);
  for (const button of document.querySelectorAll('[data-category]')) {
    const selected = button.dataset.category === state.category;
    button.classList.toggle('is-active', selected);
    button.setAttribute('aria-pressed', String(selected));
  }
  $('#district-filter').value = state.district;
  $('#sort-filter').value = state.sort;
  $('#clear-input').hidden = !input.value;
  $('#reset-filters').hidden = !state.query && state.category === 'all' && state.district === 'all' && !state.saved;
}
function updateUrl() {
  const params = new URLSearchParams();
  if (state.query) params.set('q', state.query);
  if (state.category !== 'all') params.set('category', state.category);
  if (state.district !== 'all') params.set('district', state.district);
  if (state.saved) params.set('view', 'saved');
  if (state.sort !== 'recommended') params.set('sort', state.sort);
  history.replaceState(null, '', '/' + (params.size ? '?' + params : ''));
}
function renderPlaces() {
  updateControls();
  const total = state.places.length;
  const visible = state.places.slice(0, state.visible);
  grid.innerHTML = visible.map((place, index) => {
    const saved = savedIds.has(place.id);
    return `<article class="place-card fade-in" style="animation-delay:${Math.min(index, 5) * 35}ms"><div class="place-photo"><button type="button" class="place-open" data-open="${place.id}" aria-label="${escapeHtml(place.name)} 상세 보기"><img src="${escapeHtml(place.image)}" alt="${escapeHtml(place.category_label)}의 분위기를 담은 사진" loading="lazy"></button>${place.featured ? '<span class="place-badge">EDITOR’S PICK</span>' : ''}<button type="button" class="bookmark${saved ? ' is-saved' : ''}" data-save="${place.id}" aria-label="${escapeHtml(place.name)} ${saved ? '저장 취소' : '저장'}" aria-pressed="${saved}">${BOOKMARK}</button></div><div class="place-body"><div class="place-meta"><span>${escapeHtml(place.district)}</span><span class="meta-dot"></span><span>${escapeHtml(place.category_label)}</span></div><button type="button" class="place-title" data-open="${place.id}">${escapeHtml(place.name)}</button><p class="place-description">${escapeHtml(place.description)}</p><div class="place-tags">${place.tags.slice(1, 3).map(tag => `<span>#${escapeHtml(tag.replace(/ /g,''))}</span>`).join('')}</div></div></article>`;
  }).join('');
  grid.setAttribute('aria-busy', 'false');
  grid.hidden = total === 0;
  $('#empty-state').hidden = total !== 0;
  $('#empty-heading').textContent = state.saved && savedIds.size === 0 ? '다음에 가고 싶은 곳을 담아보세요' : '아직 발견하지 못한 곳이에요';
  $('#empty-message').textContent = state.saved && savedIds.size === 0 ? '마음에 드는 장소의 책갈피를 누르면 여기에 모아둘게요.' : '다른 검색어나 동네로 다시 찾아볼까요?';
  $('#results-count').innerHTML = `발견한 공간 <b>${total}</b>`;
  const heading = state.saved ? '저장해 둔 나만의 틈' : state.query ? `‘${state.query}’의 발견` : '마음이 가는 곳으로';
  $('#results-heading').textContent = heading;
  const dot = document.createElement('span'); dot.className = 'heading-dot'; dot.textContent = '.';
  $('#results-heading').append(dot);
  $('#results-description').textContent = state.saved ? '다음에 가고 싶은 마음을 차곡차곡 모아두세요.' : state.query ? '당신의 한 단어에서 시작된, 새로운 장소들이에요.' : '잠깐의 여유를 채워줄, 취향 좋은 공간을 모았어요.';
  $('#load-more').hidden = state.visible >= total;
  $('#page-count').textContent = total ? `${Math.min(state.visible,total)} / ${total} PLACES` : '';
  $('#search-announcement').textContent = `${state.query ? state.query + ', ' : ''}${total}개의 장소를 찾았습니다.`;
}
async function loadPlaces(scroll = false) {
  const version = ++resultVersion;
  resultController?.abort();
  resultController = new AbortController();
  updateControls(); updateUrl();
  grid.setAttribute('aria-busy','true');
  const params = new URLSearchParams({q:state.query, category:state.category, district:state.district, sort:state.sort});
  if (state.saved) params.set('saved', [...savedIds].join(','));
  try {
    const response = await fetch('/api/search?' + params, {signal:resultController.signal});
    if (!response.ok) throw new Error('search unavailable');
    const data = await response.json();
    if (version !== resultVersion) return;
    state.places = data.places;
    renderPlaces();
    if (scroll) $('#discover').scrollIntoView({behavior:'smooth',block:'start'});
  } catch (error) {
    if (error.name === 'AbortError' || version !== resultVersion) return;
    state.places = [];
    renderPlaces();
    $('#empty-heading').textContent = '잠시, 길을 고르고 있어요';
    $('#empty-message').textContent = '연결을 확인하고 다시 찾아주세요.';
    $('#empty-reset').textContent = '다시 불러오기 ↗';
  }
}
function submitSearch(query) {
  state.query = query.trim().slice(0,80);
  input.value = state.query;
  state.visible = 6;
  clearTimeout(suggestTimer); ++suggestionVersion; suggestionController?.abort(); closeSuggestions();
  remember(state.query);
  document.title = state.query ? `${state.query} — 틈` : '틈 — 일상과 발견 사이';
  loadPlaces(true);
}
function resetAll() {
  Object.assign(state, {query:'',category:'all',district:'all',sort:'recommended',saved:false,visible:6});
  input.value = '';
  closeSuggestions();
  $('#empty-reset').textContent = '모든 장소 둘러보기 ↗';
  document.title = '틈 — 일상과 발견 사이';
  loadPlaces();
}
function toggleSave(id) {
  const wasSaved = savedIds.has(id);
  if (wasSaved) savedIds.delete(id); else savedIds.add(id);
  const persisted = persist('teum:saved', [...savedIds]);
  toast(wasSaved ? '저장한 장소에서 꺼냈어요.' : persisted ? '다음에 가고 싶은 곳에 담았어요.' : '지금 열린 창에 장소를 담았어요.');
  if (state.saved) loadPlaces(); else renderPlaces();
  if (dialog.open && currentPlace?.id === id) $('.dialog-save').textContent = savedIds.has(id) ? '저장 취소' : '장소 저장하기';
}
function openPlace(id) {
  const place = state.places.find(item => item.id === id);
  if (!place) return;
  currentPlace = place;
  $('#dialog-content').innerHTML = `<img class="dialog-photo" src="${escapeHtml(place.image)}" alt="${escapeHtml(place.category_label)}의 분위기"><div class="dialog-body"><div class="place-meta"><span>${escapeHtml(place.district)}</span><span class="meta-dot"></span><span>${escapeHtml(place.category_label)}</span></div><h2 id="dialog-title">${escapeHtml(place.name)}</h2><p class="dialog-description">${escapeHtml(place.description)}</p><p class="dialog-note">${escapeHtml(place.editor_note)}</p><div class="place-tags">${place.tags.map(tag => `<span>#${escapeHtml(tag.replace(/ /g,''))}</span>`).join('')}</div><div class="dialog-actions"><button type="button" class="dialog-save">${savedIds.has(id) ? '저장 취소' : '장소 저장하기'}</button><button type="button" class="dialog-similar">비슷한 곳 발견하기 ↗</button></div></div>`;
  $('.dialog-save').addEventListener('click', () => toggleSave(id));
  $('.dialog-similar').addEventListener('click', () => {
    dialog.close(); state.category = place.category; state.query=''; input.value=''; state.visible=6; loadPlaces(true);
  });
  dialog.showModal();
  document.body.style.overflow = 'hidden';
}

$('#search-form').addEventListener('submit', event => {event.preventDefault(); if(!composing) submitSearch(input.value);});
input.addEventListener('input', scheduleSuggestions);
input.addEventListener('focus', fetchSuggestions);
input.addEventListener('compositionstart', () => { composing=true; clearTimeout(suggestTimer); ++suggestionVersion; suggestionController?.abort(); closeSuggestions(); });
input.addEventListener('compositionend', () => { composing=false; scheduleSuggestions(); });
input.addEventListener('keydown', event => {
  if (composing || event.isComposing || event.keyCode === 229) return;
  if (event.key === 'Escape') { clearTimeout(suggestTimer); ++suggestionVersion; suggestionController?.abort(); closeSuggestions(); }
  else if (event.key === 'ArrowDown' || event.key === 'ArrowUp') {
    event.preventDefault();
    if (panel.hidden) fetchSuggestions(); else moveOption(event.key === 'ArrowDown' ? 1 : -1);
  } else if (event.key === 'Enter' && !panel.hidden && activeOption >= 0) { event.preventDefault(); selectSuggestion(activeOption); }
});
list.addEventListener('pointerdown', event => {if(event.target.closest('[data-index]')) event.preventDefault();});
list.addEventListener('click', event => {const option=event.target.closest('[data-index]'); if(option) selectSuggestion(Number(option.dataset.index));});
document.addEventListener('pointerdown', event => {if(!$('#search-container').contains(event.target)){clearTimeout(suggestTimer);++suggestionVersion; suggestionController?.abort(); closeSuggestions();}});
document.addEventListener('keydown', event => {if(event.key === '/' && !/INPUT|TEXTAREA|SELECT/.test(document.activeElement.tagName) && !dialog.open){event.preventDefault();input.focus();}});
$('#clear-recent').addEventListener('click', () => {recent=[];persist('teum:recent',recent);input.focus();fetchSuggestions();});
$('#clear-input').addEventListener('click', () => {input.value='';$('#clear-input').hidden=true;input.focus();fetchSuggestions();});
$('#category-tabs').addEventListener('click', event => {const button=event.target.closest('[data-category]');if(button){state.category=button.dataset.category;state.visible=6;loadPlaces();}});
$('#district-filter').addEventListener('change', event => {state.district=event.target.value;state.visible=6;loadPlaces();});
$('#sort-filter').addEventListener('change', event => {state.sort=event.target.value;loadPlaces();});
document.querySelectorAll('[data-query]').forEach(button => button.addEventListener('click', () => {state.saved=false;state.category='all';state.district='all';input.value=button.dataset.query;submitSearch(button.dataset.query);}));
grid.addEventListener('click', event => {const save=event.target.closest('[data-save]');const open=event.target.closest('[data-open]');if(save)toggleSave(Number(save.dataset.save));else if(open)openPlace(Number(open.dataset.open));});
$('#load-more').addEventListener('click', () => {state.visible+=6;renderPlaces();});
$('#reset-filters').addEventListener('click', resetAll);
$('#empty-reset').addEventListener('click', resetAll);
$('#nav-discover').addEventListener('click', resetAll);
$('#nav-saved').addEventListener('click', () => {state.saved=!state.saved;state.query='';state.category='all';state.district='all';state.visible=6;input.value='';closeSuggestions();loadPlaces(true);});
$('#dialog-close').addEventListener('click', () => dialog.close());
dialog.addEventListener('close', () => {document.body.style.overflow='';});
dialog.addEventListener('click', event => {if(event.target===dialog){const rect=dialog.getBoundingClientRect();if(event.clientX<rect.left||event.clientX>rect.right||event.clientY<rect.top||event.clientY>rect.bottom)dialog.close();}});

async function init() {
  try {
    const response = await fetch('/api/config');
    if (!response.ok) throw new Error('config unavailable');
    const config = await response.json();
    for (const district of config.districts) {
      const option = document.createElement('option'); option.value=district; option.textContent=district; $('#district-filter').append(option);
    }
    const params = new URLSearchParams(location.search);
    state.query=(params.get('q')||'').slice(0,80);
    state.saved=params.get('view')==='saved';
    if(config.categories.some(category=>category.id===params.get('category')))state.category=params.get('category');
    if(config.districts.includes(params.get('district')))state.district=params.get('district');
    if(params.get('sort')==='name')state.sort='name';
    input.value=state.query;
  } catch { /* 검색 API가 준비되어 있다면 기본 상태로 계속 시도한다. */ }
  await loadPlaces();
}
init();
