/* 실제 app.js를 격리된 DOM 모형에서 실행하는 입력 이벤트 회귀 테스트.
 * 브라우저 자동화가 아니라 단위 테스트다. 특히 compositionend 없이 응답이
 * 표시되는지 검사해서 API 테스트만으로 찾을 수 없던 한글 입력 버그를 재현한다. */
const {test} = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../web/teum/app.js'), 'utf8');
const settle = () => new Promise(resolve => setImmediate(resolve));
const visibleSuggestions = fixture => fixture.element('#suggestions').innerHTML.replace(/<[^>]*>/g, '');

class Element {
  constructor() {
    this.value = ''; this.hidden = true; this.innerHTML = ''; this.textContent = '';
    this.open = false; this.tagName = 'INPUT'; this.style = {};
    this.listeners = new Map(); this.attributes = new Map();
    this.classList = {add(){}, remove(){}, toggle(){}};
  }
  addEventListener(type, handler) {
    if (!this.listeners.has(type)) this.listeners.set(type, []);
    this.listeners.get(type).push(handler);
  }
  emit(type, extra = {}) {
    const event = {target:this, preventDefault(){this.defaultPrevented=true;}, ...extra};
    for (const handler of this.listeners.get(type) || []) handler(event);
    return event;
  }
  setAttribute(name, value) { this.attributes.set(name, value); }
  getAttribute(name) { return this.attributes.get(name); }
  removeAttribute(name) { this.attributes.delete(name); }
  append() {}
  scrollIntoView() {}
  querySelectorAll() { return []; }
}

async function fixture() {
  const nodes = new Map();
  const element = selector => {
    if (!nodes.has(selector)) nodes.set(selector, new Element());
    return nodes.get(selector);
  };
  const input = element('#search-input');
  const document = new Element();
  document.activeElement = input;
  document.querySelector = element;
  document.querySelectorAll = () => [];
  document.createElement = () => new Element();
  document.body = new Element();
  const requests = [], searches = [], timers = new Map();
  let now = 0, nextTimer = 0;
  const context = vm.createContext({
    document, console, URLSearchParams, AbortController,
    location:{search:''}, history:{replaceState(){}},
    localStorage:{getItem(){return null;},setItem(){}},
    setTimeout(callback, delay) { const id=++nextTimer; timers.set(id,{at:now+delay,callback});return id; },
    clearTimeout(id) {timers.delete(id);},
    fetch(url, options={}) {
      if (url === '/api/config') return Promise.resolve({ok:true,json:async()=>({categories:[{id:'all'}],districts:[]})});
      if (url.startsWith('/api/search?')) {
        searches.push(url);
        return Promise.resolve({ok:true,json:async()=>({places:[],total:0})});
      }
      return new Promise(resolve => {
        requests.push({query:new URL(url,'http://test').searchParams.get('q'), signal:options.signal,
          complete(text) {resolve({ok:true,json:async()=>({suggestions:[{text,kind:'키워드'}]})});}});
      });
    },
  });
  vm.runInContext(source, context, {filename:'app.js'});
  await settle();
  return {input,document,element,requests,searches,
    async advance(ms) {
      now += ms;
      for (const [id,timer] of [...timers]) {
        if (timer.at <= now) {timers.delete(id);timer.callback();}
      }
      await settle();
    },
  };
}

test('성 한 글자는 조합 중이어도 공백/다음 글자 없이 추천을 표시한다', async () => {
  const f = await fixture();
  f.input.emit('compositionstart');
  for (const syllable of ['ㅅ','서']) {
    f.input.value=syllable;f.input.emit('input',{isComposing:true});await f.advance(40);
  }
  f.input.value='성';f.input.emit('input',{isComposing:true});
  await f.advance(89);
  assert.equal(f.requests.length,0);
  await f.advance(1);
  assert.equal(f.requests.length,1);
  assert.equal(f.requests[0].query,'성');
  f.requests[0].complete('성수 카페');
  await settle();
  assert.equal(f.element('#suggest-panel').hidden,false);
  assert.equal(f.input.getAttribute('aria-expanded'),'true');
  assert.match(visibleSuggestions(f),/성수 카페/);
  const searchCount=f.searches.length;
  f.element('#search-form').emit('submit');
  await settle();
  assert.equal(f.searches.length,searchCount,'IME 확정 중 Enter가 장소 검색을 실행하면 안 됨');
});

test('compositionupdate만 발생하는 입력 경로도 처리한다', async () => {
  const f=await fixture();
  f.input.emit('compositionstart');f.input.value='성';f.input.emit('compositionupdate');
  await f.advance(90);
  assert.equal(f.requests[0].query,'성');
  f.requests[0].complete('성수 산책');await settle();
  assert.equal(f.element('#suggest-panel').hidden,false);
});

test('조합 중 오래된 응답이 최신 검색 후보를 덮어쓰지 않는다', async () => {
  const f=await fixture();
  f.input.emit('compositionstart');f.input.value='성';f.input.emit('input');await f.advance(90);
  f.input.value='성수';f.input.emit('input');await f.advance(90);
  assert.equal(f.requests[0].signal.aborted,true);
  f.requests[1].complete('성수 카페');await settle();
  f.requests[0].complete('성수 산책');await settle();
  assert.match(visibleSuggestions(f),/성수 카페/);
  assert.doesNotMatch(visibleSuggestions(f),/성수 산책/);
});

test('조합 완료와 뒤따르는 input은 하나의 debounce 요청으로 합친다', async () => {
  const f=await fixture();
  f.input.emit('compositionstart');f.input.value='성';f.input.emit('compositionupdate');
  f.input.emit('input');f.input.emit('compositionend');f.input.emit('input');
  await f.advance(90);
  assert.equal(f.requests.length,1);
  assert.equal(f.requests[0].query,'성');
});

test('Escape는 예약된 요청을 취소해 추천 창을 다시 열지 않는다', async () => {
  const f=await fixture();
  f.input.value='성';f.input.emit('input');f.input.emit('keydown',{key:'Escape'});
  await f.advance(90);
  assert.equal(f.requests.length,0);
  assert.equal(f.element('#suggest-panel').hidden,true);
});

test('포커스를 떠난 뒤 도착한 응답은 표시하지 않는다', async () => {
  const f=await fixture();
  f.input.value='성';f.input.emit('input');await f.advance(90);
  f.document.activeElement=new Element();
  f.requests[0].complete('성수 카페');await settle();
  assert.equal(f.element('#suggest-panel').hidden,true);
});
