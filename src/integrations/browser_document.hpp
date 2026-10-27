#pragma once
#include <nlohmann/json.hpp>
#include <string>
namespace openxmb::integrations::detail {
inline std::string browser_document_script(const std::string& options_code,bool osk) {
  return "(()=>{const optionsCode="+nlohmann::json(options_code).dump()+";const oskEnabled="+(osk?"true;":"false;")+R"JS(
const config={openxmbConfig:{code:optionsCode,osk:oskEnabled}};
window.__openxmbOptionsCode=optionsCode;
window.__openxmbOSKEnabled=oskEnabled;
if(window.__openxmbDocumentInstalled){postMessage(config,'*');return;}
window.__openxmbDocumentInstalled=true;
const frameId=crypto.randomUUID?crypto.randomUUID():Math.random().toString(36).slice(2);
let serial=0,lastTarget=null,waitingId=null,restoring=false;
const requests=new Map();
function relay(message){for(let i=0;i<frames.length;i++)frames[i].postMessage(message,'*');}
function editable(node){
  if(!(node instanceof Element))return null;
  const e=node.closest('input,textarea,[contenteditable]');
  if(!e||e.disabled||e.readOnly)return null;
  if(e instanceof HTMLInputElement&&!['text','search','url','email','tel','password','number'].includes(e.type))return null;
  if(!(e instanceof HTMLInputElement)&&!(e instanceof HTMLTextAreaElement)&&!e.isContentEditable)return null;
  return e;
}
function request(e,explicit){
  if(!e||!window.__openxmbOSKEnabled||restoring)return;
  if(waitingId&&requests.get(waitingId)?.element===e)return;
  if(!explicit&&lastTarget===e)return;
  lastTarget=e;
  const id=frameId+':'+(++serial),password=e instanceof HTMLInputElement&&e.type==='password';
  // Never read an existing password, even transiently for a request payload.
  const value=password?'':Array.from(e.isContentEditable?e.textContent:e.value).slice(0,4096).join('');
  const label=Array.from(e.getAttribute('aria-label')||e.getAttribute('placeholder')||e.id||'Text').slice(0,100).join('');
  const selection={start:e.selectionStart,end:e.selectionEnd};
  requests.set(id,{element:e,selection});waitingId=id;
  const payload={type:'openxmb-edit',id,password,value,label,multiline:e instanceof HTMLTextAreaElement||e.isContentEditable};
  // Frame-specific native messaging keeps cross-origin editable contents out
  // of ancestor windows. Replies are delivered only to this same native frame.
  chrome.webview.postMessage(JSON.stringify(payload));
}
function reply(data){
  const saved=requests.get(data.id);requests.clear();waitingId=null;
  if(!saved||!saved.element.isConnected)return;
  const e=saved.element;restoring=true;lastTarget=e;
  if(data.accepted){
    let value=String(data.text??'');
    if(typeof e.maxLength==='number'&&e.maxLength>=0)value=value.slice(0,e.maxLength);
    if(e.isContentEditable)e.textContent=value;
    else {
      const prototype=e instanceof HTMLTextAreaElement?HTMLTextAreaElement.prototype:HTMLInputElement.prototype;
      Object.getOwnPropertyDescriptor(prototype,'value').set.call(e,value);
    }
    e.dispatchEvent(new InputEvent('input',{bubbles:true,composed:true,inputType:'insertReplacementText',data:value}));
    e.dispatchEvent(new Event('change',{bubbles:true}));
  }
  e.focus({preventScroll:true});
  try{const n=e.value?.length??0;e.setSelectionRange(data.accepted?n:saved.selection.start,data.accepted?n:saved.selection.end);}catch(_){}
  queueMicrotask(()=>{restoring=false;});
}
function message(data){
  if(!data||typeof data!=='object')return;
  if(data.openxmbConfig){
    window.__openxmbOptionsCode=data.openxmbConfig.code;
    window.__openxmbOSKEnabled=!!data.openxmbConfig.osk;
    if(!window.__openxmbOSKEnabled){requests.clear();waitingId=null;}
    relay(data);
  }
  if(data.openxmbEditReply)reply(data.openxmbEditReply);
}
addEventListener('message',e=>{
  if(e.data==='openxmb-options'&&window===top)chrome.webview.postMessage('openxmb-options');
  else message(e.data);
});
chrome.webview.addEventListener('message',e=>message(e.data));
addEventListener('keydown',e=>{
  if(e.code===window.__openxmbOptionsCode){e.preventDefault();e.stopImmediatePropagation();chrome.webview.postMessage('openxmb-options');}
},true);
addEventListener('focusin',e=>request(editable(e.composedPath()[0]),false),true);
addEventListener('pointerdown',e=>request(editable(e.composedPath()[0]),true),true);
message(config);
})();)JS";
}
}
