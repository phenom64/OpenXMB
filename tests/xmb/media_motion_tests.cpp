#include "../../src/app/components/media_motion.hpp"
#include <cmath>
#include <stdexcept>
#include <iostream>
void check(bool b,const char* m){if(!b)throw std::runtime_error(m);}
int main()try{using namespace app::media_motion;
 for(unsigned depth:{1u,2u,10u})for(double p:{0.0,0.1,0.5,1.0}){
  check(category(depth,depth+1,p)==1,"deeper folder must not replay category collapse");
  check(category(depth+1,depth,p)==1,"nested Back must keep category anchored");}
 check(category(0,1,0)==0&&category(0,1,1)==1,"root entry collapses once");
 check(category(1,0,0)==1&&category(1,0,1)==0,"return to root restores category rail");
 for(double p:{0.0,0.2,0.5,0.8,1.0}){auto f=layers(false,p),b=layers(true,p);
  check(f.outgoing_x<=0&&f.incoming_x>=0,"forward glides old left/new from right");
  check(b.outgoing_x>=0&&b.incoming_x<=0,"Back reverses layer travel");
  check(std::abs(f.outgoing_alpha+f.incoming_alpha-1)<1e-6,"crossfade preserves complementary coverage");}
 check(backdrop(12,true)==0&&backdrop(.2f,true)==0,"hidden folder blur and dim must not leak into opaque player");
 check(backdrop(12,false)==12,"visible folder backdrop retained");
 std::cout<<"Media category anchoring, reversible glides and opaque-scene backdrop passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
