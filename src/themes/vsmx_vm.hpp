#pragma once

#include "openxmb/themes/library.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace openxmb::themes::vsmx {
struct Object;
struct Value;
using Native = std::function<Value(const Value&, std::span<const Value>)>;
struct Value {
    enum Kind { undefined, null, number, boolean, string, object, function, native, reference, method } kind=undefined;
    float numeric{};
    std::string text;
    std::shared_ptr<Object> obj;
    std::shared_ptr<Native> callback;
    std::shared_ptr<Value> receiver;
    std::uint32_t entry{};
    Value()=default;
    Value(float n):kind(number),numeric(n){}
    Value(double n):Value(static_cast<float>(n)){}
    Value(int n):Value(static_cast<float>(n)){}
    Value(bool b):kind(boolean),numeric(b?1.0F:0.0F){}
    Value(std::string s):kind(string),text(std::move(s)){}
    Value(const char* s):Value(std::string(s)){}
    explicit Value(std::shared_ptr<Object> p):kind(object),obj(std::move(p)){}
    explicit Value(Native fn):kind(native),callback(std::make_shared<Native>(std::move(fn))){}
};
struct Object {
    std::map<std::string,Value> properties;
    std::vector<Value> items;
    bool array=false;
};
inline Value get(const Value& value,std::string_view key);
inline Value deref(Value value) {
    for(int i=0;i<16 && value.kind==Value::reference;++i) {
        Value parent(value.obj); value=get(parent,value.text);
    }
    return value;
}
inline std::optional<std::size_t> index(std::string_view key) {
    if(key.empty() || key.size()>8) return {};
    std::size_t n=0;
    for(char c:key) { if(c<'0'||c>'9') return {}; n=n*10+static_cast<unsigned>(c-'0'); }
    if(n>4095) throw ThemeError("VSMX array index exceeds its budget");
    return n;
}
inline Value get(const Value& raw,std::string_view key) {
    const auto value=deref(raw);
    if(value.kind==Value::string && key=="length") return static_cast<int>(value.text.size());
    if(value.kind!=Value::object || !value.obj) return {};
    if(value.obj->array) {
        if(key=="length") return static_cast<int>(value.obj->items.size());
        if(auto n=index(key)) return *n<value.obj->items.size()?value.obj->items[*n]:Value{};
    }
    const auto found=value.obj->properties.find(std::string(key));
    return found==value.obj->properties.end()?Value{}:found->second;
}
inline void set(const Value& raw,std::string_view key,Value value) {
    const auto parent=deref(raw);
    if(parent.kind!=Value::object || !parent.obj) throw ThemeError("VSMX assignment target is not an object");
    if(parent.obj->array) {
        if(key=="length") {
            const auto n=deref(value);
            if(!std::isfinite(n.numeric)||n.numeric<0||n.numeric>4096) throw ThemeError("VSMX array exceeds its budget");
            parent.obj->items.resize(static_cast<std::size_t>(n.numeric)); return;
        }
        if(auto n=index(key)) {
            if(parent.obj->items.size()<=*n) parent.obj->items.resize(*n+1);
            const auto owner=parent.obj->properties.find("__owner");
            if(owner!=parent.obj->properties.end()&&value.obj&&value.obj->properties.contains("__timer"))
                value.obj->properties["__owner"]=owner->second;
            parent.obj->items[*n]=std::move(value); return;
        }
    }
    if(parent.obj->properties.size()>=1024 && !parent.obj->properties.contains(std::string(key))) throw ThemeError("VSMX object exceeds its property budget");
    parent.obj->properties[std::string(key)]=std::move(value);
}
inline float number(Value value) {
    value=deref(std::move(value));
    if(value.kind==Value::number || value.kind==Value::boolean) return value.numeric;
    if(value.kind==Value::null) return 0;
    if(value.kind==Value::string) { try { return std::stof(value.text); } catch(...) { return 0; } }
    return 0;
}
inline bool truth(Value value) {
    value=deref(std::move(value));
    if(value.kind==Value::undefined || value.kind==Value::null) return false;
    if(value.kind==Value::number || value.kind==Value::boolean) return value.numeric!=0 && !std::isnan(value.numeric);
    if(value.kind==Value::string) return !value.text.empty();
    return true;
}
inline std::string string(Value value) {
    value=deref(std::move(value));
    if(value.kind==Value::string) return value.text;
    if(value.kind==Value::undefined) return "undefined";
    if(value.kind==Value::null) return "null";
    if(value.kind==Value::boolean) return value.numeric?"true":"false";
    if(value.kind==Value::number) {
        if(std::isfinite(value.numeric)&&std::abs(value.numeric)<1.0e15F&&std::trunc(value.numeric)==value.numeric)
            return std::to_string(static_cast<std::int64_t>(value.numeric));
        return std::to_string(value.numeric);
    }
    return "[object]";
}

class VM {
public:
    Value globals;
    std::vector<std::string> warnings;
    explicit VM(std::span<const std::uint8_t> bytes) {
        globals=make_object();
        if(bytes.empty()) return;
        auto word=[&](std::size_t at) {
            if(at>bytes.size() || bytes.size()-at<4) throw ThemeError("Truncated VSMX header");
            return std::uint32_t(bytes[at])|(std::uint32_t(bytes[at+1])<<8)|(std::uint32_t(bytes[at+2])<<16)|(std::uint32_t(bytes[at+3])<<24);
        };
        if(bytes.size()<48 || std::string_view(reinterpret_cast<const char*>(bytes.data()),4)!="VSMX") throw ThemeError("Invalid VSMX signature");
        auto section=[&](std::size_t at) {
            const auto offset=word(at),size=word(at+4);
            if(offset>bytes.size() || size>bytes.size()-offset) throw ThemeError("VSMX section exceeds file bounds");
            return bytes.subspan(offset,size);
        };
        const auto code=section(8);
        if(code.size()%8 || code.size()>2*1024*1024) throw ThemeError("Invalid VSMX instruction count");
        for(std::size_t i=0;i<code.size();i+=8) {
            const auto le=[&](std::size_t j){return std::uint32_t(code[j])|(std::uint32_t(code[j+1])<<8)|(std::uint32_t(code[j+2])<<16)|(std::uint32_t(code[j+3])<<24);};
            code_.push_back({le(i),le(i+4)});
        }
        text_=strings(section(16)); props_=strings(section(28)); names_=strings(section(40));
    }
    ~VM() {
        // Script objects may refer to themselves. Breaking their property links
        // releases cycles when a theme is replaced, without exposing a GC API.
        for(auto& weak:objects_) if(auto object=weak.lock()) {object->properties.clear();object->items.clear();}
    }
    Value make_object(bool array=false) {
        if(objects_.size()>=8192) {
            std::erase_if(objects_,[](auto& p){return p.expired();});
            if(objects_.size()>=8192) throw ThemeError("VSMX live object budget exhausted");
        }
        auto object=std::make_shared<Object>();object->array=array;objects_.push_back(object);return Value(std::move(object));
    }
    Value array(std::span<const float> values) {
        auto result=make_object(true);result.obj->items.reserve(values.size());
        for(float value:values) result.obj->items.emplace_back(value);
        return result;
    }
    Value from_json(const nlohmann::json& json,unsigned depth=0) {
        if(depth>32) throw ThemeError("VSMX host value nesting exceeds budget");
        if(json.is_null()) {Value v;v.kind=Value::null;return v;}
        if(json.is_boolean()) return json.get<bool>();
        if(json.is_number()) return json.get<float>();
        if(json.is_string()) return json.get<std::string>();
        auto v=make_object(json.is_array());
        if(json.size()>4096) throw ThemeError("VSMX host container exceeds budget");
        if(json.is_array()) for(const auto& x:json) v.obj->items.push_back(from_json(x,depth+1));
        else for(auto it=json.begin();it!=json.end();++it) set(v,it.key(),from_json(it.value(),depth+1));
        return v;
    }
    nlohmann::json to_json(Value v,unsigned depth=0) const {
        v=deref(std::move(v));
        if(depth>16) return nullptr;
        if(v.kind==Value::number) return std::isfinite(v.numeric)?nlohmann::json(v.numeric):nlohmann::json{};
        if(v.kind==Value::boolean) return v.numeric!=0;
        if(v.kind==Value::string) return v.text;
        if(v.kind!=Value::object || !v.obj) return nullptr;
        nlohmann::json out=v.obj->array?nlohmann::json::array():nlohmann::json::object();
        if(v.obj->array) for(const auto& x:v.obj->items) out.push_back(to_json(x,depth+1));
        else for(const auto& [key,x]:v.obj->properties) if(x.kind!=Value::native && x.kind!=Value::function && key!="timer") out[key]=to_json(x,depth+1);
        return out;
    }
    void begin_tick() noexcept { budget_=200000; }
    void run() { begin_tick(); if(!code_.empty()) execute(0,{},{}); }
    Value invoke(Value callable,const Value& self,std::span<const Value> args) {
        callable=deref(std::move(callable));
        if(callable.kind==Value::function) return execute(callable.entry,self,args);
        if(callable.kind==Value::native && callable.callback) return (*callable.callback)(self,args);
        return {};
    }
private:
    struct Instruction {std::uint32_t op,arg;};
    std::vector<Instruction> code_;
    std::vector<std::string> text_,props_,names_;
    std::vector<std::weak_ptr<Object>> objects_;
    std::size_t budget_=200000;
    unsigned depth_=0;
    static std::vector<std::string> strings(std::span<const std::uint8_t> data) {
        if(data.size()>1024*1024) throw ThemeError("VSMX string table exceeds budget");
        std::size_t oddZeros=0;
        for(std::size_t i=1;i<data.size();i+=2)if(data[i]==0)++oddZeros;
        const bool wide=data.size()>1 && data.size()%2==0 && oddZeros*4>=(data.size()/2)*3;
        std::vector<std::string> out;std::string text;
        for(std::size_t i=0;i<data.size();i+=wide?2:1) {
            unsigned c=data[i];if(wide) {if(i+1==data.size()) throw ThemeError("Truncated UTF-16 VSMX string");c|=unsigned(data[i+1])<<8;}
            if(c==0) {out.push_back(std::move(text));text.clear();}
            else if(c<0x80) text.push_back(static_cast<char>(c));
            else if(c<0x800) {text.push_back(static_cast<char>(0xc0|(c>>6)));text.push_back(static_cast<char>(0x80|(c&63)));}
            else {text.push_back(static_cast<char>(0xe0|(c>>12)));text.push_back(static_cast<char>(0x80|((c>>6)&63)));text.push_back(static_cast<char>(0x80|(c&63)));}
            if(text.size()>8192 || out.size()>65536) throw ThemeError("VSMX strings exceed budget");
        }
        if(!text.empty()) out.push_back(std::move(text));return out;
    }
    static const std::string& at(const std::vector<std::string>& values,std::uint32_t i) {
        if(i>=values.size()) throw ThemeError("Invalid VSMX string index");return values[i];
    }
    static std::int32_t integer(Value v) {
        double n=number(std::move(v)); if(!std::isfinite(n)) return 0;
        n=std::fmod(std::trunc(n),4294967296.0);if(n<0)n+=4294967296.0;
        return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(n));
    }
    Value binary(unsigned op,Value left,Value right) {
        left=deref(std::move(left));right=deref(std::move(right));
        if((op==0xe||op==0x10) && left.obj&&right.obj&&left.obj->array&&right.obj->array) {
            if(left.obj->items.size()!=right.obj->items.size())return false;
            for(std::size_t i=0;i<left.obj->items.size();++i) if(!truth(binary(op,left.obj->items[i],right.obj->items[i])))return false;
            return true;
        }
        if(op==2 && (left.kind==Value::string || right.kind==Value::string)) {
            auto text=string(left)+string(right);if(text.size()>65536)throw ThemeError("VSMX string result exceeds budget");return text;
        }
        if(op>=2 && op<=5 && ((left.obj&&left.obj->array)||(right.obj&&right.obj->array))) {
            auto out=make_object(true); const auto n=left.obj&&left.obj->array?left.obj->items.size():right.obj->items.size();
            for(std::size_t i=0;i<n;++i) out.obj->items.push_back(binary(op,left.obj&&left.obj->array?get(left,std::to_string(i)):left,
                right.obj&&right.obj->array?get(right,std::to_string(i)):right));
            return out;
        }
        const float l=number(left),r=number(right);
        switch(op) {
            case 2:return l+r;case 3:return l-r;case 4:return l*r;case 5:return l/r;case 6:return std::fmod(l,r);
            case 0xe:case 0x10:return left.kind==Value::object||right.kind==Value::object ? left.obj==right.obj :
                left.kind==Value::string||right.kind==Value::string ? string(left)==string(right) : l==r;
            case 0xf:case 0x11:return !truth(binary(0xe,left,right));
            case 0x12:return l<r;case 0x13:return l<=r;case 0x14:return l>=r;case 0x15:return l>r;
            case 0x19:return integer(left)&integer(right);case 0x1a:return integer(left)^integer(right);case 0x1b:return integer(left)|integer(right);
            case 0x1d:return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(integer(left))<<(static_cast<unsigned>(integer(right))&31));
            case 0x1e:return integer(left)>>(static_cast<unsigned>(integer(right))&31);
            case 0x1f:return static_cast<float>(static_cast<std::uint32_t>(integer(left))>>(static_cast<unsigned>(integer(right))&31));
            default:throw ThemeError("Unsupported VSMX opcode "+std::to_string(op));
        }
    }
    Value call_method(const Value& method,std::span<const Value> args) {
        if(!method.receiver) return {};
        const auto self=deref(*method.receiver);
        auto fn=get(self,method.text);
        if(fn.kind==Value::function || fn.kind==Value::native) return invoke(fn,self,args);
        if(self.obj&&self.obj->array) {
            if(method.text=="push") {for(const auto& a:args)set(self,std::to_string(self.obj->items.size()),a);return static_cast<int>(self.obj->items.size());}
            if(method.text=="pop") {if(self.obj->items.empty())return {};auto v=self.obj->items.back();self.obj->items.pop_back();return v;}
            if(method.text=="slice") {
                auto out=make_object(true);auto begin=args.empty()?0:static_cast<int>(number(args[0]));
                auto end=args.size()<2?static_cast<int>(self.obj->items.size()):static_cast<int>(number(args[1]));
                if(begin<0)begin+=self.obj->items.size();if(end<0)end+=self.obj->items.size();
                begin=std::clamp(begin,0,static_cast<int>(self.obj->items.size()));end=std::clamp(end,begin,static_cast<int>(self.obj->items.size()));
                out.obj->items.assign(self.obj->items.begin()+begin,self.obj->items.begin()+end);return out;
            }
        }
        if(method.text=="toString") return string(self);
        const auto label=method.text+"()";
        if(warnings.size()<128&&std::find(warnings.begin(),warnings.end(),label)==warnings.end())warnings.push_back(label);
        return {};
    }
    Value execute(std::uint32_t pc,const Value& self,std::span<const Value> args) {
        if(++depth_>32) {--depth_;throw ThemeError("VSMX call depth exhausted");}
        struct Depth {unsigned& n;~Depth(){--n;}} depth{depth_};
        if(args.size()>256) throw ThemeError("VSMX argument count exceeds budget");
        auto locals=make_object(true);locals.obj->items.push_back({});locals.obj->items.insert(locals.obj->items.end(),args.begin(),args.end());
        std::vector<Value> stack;stack.reserve(64);
        struct Try {std::uint32_t pc;std::size_t depth;};std::vector<Try> tries;
        const auto raw=[&]() {if(stack.empty())return Value{};auto v=stack.back();stack.pop_back();return v;};
        const auto pop=[&]() {return deref(raw());};
        const auto popn=[&](std::uint32_t count) {
            if(count>stack.size()||count>256)throw ThemeError("Invalid VSMX argument stack");
            std::vector<Value> out(stack.end()-count,stack.end());stack.resize(stack.size()-count);for(auto& v:out)v=deref(v);return out;};
        const auto ref=[](Value object,std::string key) {Value v;v.kind=Value::reference;v.obj=object.obj;v.text=std::move(key);return v;};
        while(pc<code_.size()) {
            if(budget_==0)throw ThemeError("VSMX instruction budget exhausted");--budget_;
            const auto instruction=code_[pc++];const auto op=instruction.op&255,a=instruction.arg;
            try {
                switch(op) {
                    case 0x41:if(tries.size()>=32)throw ThemeError("VSMX try depth exhausted");tries.push_back({a,stack.size()});break;
                    case 0x42:if(!tries.empty())tries.pop_back();break;
                    case 0x43:pc=a;break;case 0x44:break;
                    case 0x40:throw ThemeError("Theme script: "+string(pop()));
                    case 0x21:{auto x=raw(),y=raw();stack.push_back(x);stack.push_back(y);break;}
                    case 0x37:{auto key=string(pop());auto object=pop();if(object.obj)object.obj->properties.erase(key);stack.emplace_back(true);break;}
                    case 0x2e:stack.push_back(ref(globals,at(names_,a)));break;
                    case 0x2d:if(a>4095)throw ThemeError("VSMX local index exceeds budget");stack.push_back(ref(locals,std::to_string(a)));break;
                    case 0x2f:stack.push_back(ref(pop(),at(props_,a)));break;
                    case 0x30:{Value m;m.kind=Value::method;m.receiver=std::make_shared<Value>(pop());m.text=at(props_,a);stack.push_back(std::move(m));break;}
                    case 0x31:{auto v=pop(),o=pop();set(o,at(props_,a),v);stack.push_back(v);break;}
                    case 0x33:{auto v=pop();if(!stack.empty())set(deref(stack.back()),at(props_,a),v);break;}
                    case 0x01:{auto v=pop(),r=raw();if(r.kind==Value::reference)set(Value(r.obj),r.text,v);stack.push_back(v);break;}
                    case 0x36:{auto v=pop();auto key=string(pop());auto o=pop();set(o,key,v);stack.push_back(v);break;}
                    case 0x48:if(stack.size()>=2)stack.push_back(get(deref(stack[stack.size()-2]),string(deref(stack.back()))));break;
                    case 0x34:{auto key=string(pop());stack.push_back(ref(pop(),key));break;}
                    case 0x23:stack.emplace_back();break;case 0x24:{Value v;v.kind=Value::null;stack.push_back(v);break;}
                    case 0x29:stack.push_back(make_object());break;case 0x2b:stack.push_back(make_object(true));break;
                    case 0x38:{auto v=pop();if(!stack.empty()){auto o=deref(stack.back());if(o.obj&&o.obj->array)set(o,std::to_string(o.obj->items.size()),v);}break;}
                    case 0x25:stack.emplace_back(a!=0);break;case 0x26:stack.emplace_back(std::bit_cast<std::int32_t>(a));break;
                    case 0x27:stack.emplace_back(std::bit_cast<float>(a));break;case 0x28:stack.emplace_back(at(text_,a));break;
                    case 0x2c:stack.push_back(self);break;
                    case 0x49:{auto values=popn(a);auto v=make_object(true);for(auto& x:values)v.obj->items.emplace_back(number(x));stack.push_back(v);break;}
                    case 0x2a:{if(a>=code_.size())throw ThemeError("VSMX function points outside code");Value v;v.kind=Value::function;v.entry=a;stack.push_back(v);break;}
                    case 0x3c:{auto values=popn(a);auto f=pop();stack.push_back(invoke(f,{},values));break;}
                    case 0x3d:{auto values=popn(a);auto m=raw();stack.push_back(m.kind==Value::method?call_method(m,values):Value{});break;}
                    case 0x3e:{auto values=popn(a);auto named=raw();auto constructor=deref(named);
                        if(constructor.kind!=Value::function&&constructor.kind!=Value::native){if(warnings.size()<128)warnings.push_back("new "+named.text);stack.emplace_back();break;}
                        auto object=make_object();auto result=invoke(constructor,object,values);stack.push_back(result.kind==Value::object?result:constructor.kind==Value::function?object:result);break;}
                    case 0x20:stack.push_back(stack.empty()?Value{}:stack.back());break;case 0x22:raw();break;
                    case 0x39:pc=a;break;case 0x3a:if(truth(pop()))pc=a;break;case 0x3b:if(!truth(pop()))pc=a;break;
                    case 0x3f:return pop();case 0x45:return {};case 0x46:case 0x47:break;
                    case 0x09:stack.emplace_back(!truth(pop()));break;case 0x08:stack.emplace_back(-number(pop()));break;
                    case 0x07:stack.emplace_back(number(pop()));break;case 0x1c:stack.emplace_back(~integer(pop()));break;
                    case 0xa:case 0xb:case 0xc:case 0xd:{auto r=raw();const float old=number(r),value=old+(op==0xa||op==0xc?1:-1);if(r.kind==Value::reference)set(Value(r.obj),r.text,value);stack.emplace_back(op<0xc?value:old);break;}
                    case 0x4a:case 0x4b:{if(stack.empty())break;const auto top=stack.back();auto source=top.kind==Value::method&&top.receiver?get(*top.receiver,top.text):deref(top);auto element=get(source,std::to_string(instruction.op>>8));if(op==0x4b||top.kind==Value::method)stack.push_back(element);else stack.back()=element;break;}
                    case 0x4c:{auto value=pop(),target=raw(),vector=deref(target);if(!vector.obj||!vector.obj->array)vector=make_object(true);set(vector,std::to_string(instruction.op>>8),value);if(target.kind==Value::reference)set(Value(target.obj),target.text,vector);stack.push_back(value);break;}
                    case 0x4d:{auto value=pop(),target=raw();auto object=target.kind==Value::method&&target.receiver?*target.receiver:deref(target);auto vector=get(object,at(props_,a));if(!vector.obj||!vector.obj->array)vector=make_object(true);set(vector,std::to_string(instruction.op>>8),value);set(object,at(props_,a),vector);stack.push_back(value);break;}
                    default:{auto right=pop(),left=raw();if(left.kind==Value::method&&left.receiver){auto object=*left.receiver;stack.push_back(object);stack.push_back(binary(op,get(object,left.text),right));}else stack.push_back(binary(op,deref(left),right));break;}
                }
                if(stack.size()>4096)throw ThemeError("VSMX stack budget exhausted");
                if(pc>code_.size())throw ThemeError("VSMX jump points outside code");
            } catch(const std::exception& error) {
                if(tries.empty())throw;
                const auto target=tries.back();tries.pop_back();stack.resize(target.depth);stack.emplace_back(error.what());pc=target.pc+1;
            }
        }
        return {};
    }
};
} // namespace openxmb::themes::vsmx
