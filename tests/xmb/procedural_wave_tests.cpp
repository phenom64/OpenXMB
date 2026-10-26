#include "openxmb/xmb/procedural_wave.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace {
void require(bool condition,std::string_view message) {
    if(!condition) { std::cerr<<"procedural wave: "<<message<<'\n'; std::exit(EXIT_FAILURE); }
}
}

int main() {
    using namespace openxmb::xmb;
    ProceduralWaveSettings settings;
    ProceduralWaveSampler a,b;
    a.update(36,settings); b.update(36,settings);
    require(a.frame().control==b.frame().control,"same seed and timestamp produce identical geometry");
    const auto first=a.frame().control;
    require(std::all_of(first.begin(),first.end(),[](float v){return std::isfinite(v)&&std::abs(v)<1;}),
            "default ribbon is finite and bounded");
    require(a.displacement(0.2F,0.1F)!=a.displacement(0.8F,0.9F),"ribbon has spatial structure");
    a.update(37,settings);
    require(a.frame().control!=first,"ribbon advances with its clock");
    a.update(36,settings);
    require(a.frame().control==first,"seeking back reproduces geometry without frame history");
    settings.reSyntheticDescriptorSeed=9001;
    b.update(36,settings);
    require(b.frame().control!=first,"theme seed changes the synthetic ribbon");
    require(settings.set("thickness",2.25F)&&settings.thickness==2.25F,"theme parameter reaches renderer settings");
    require(!settings.set("opacity",std::numeric_limits<float>::quiet_NaN()),"non-finite imported parameters rejected");
    require(!settings.set("unknown",1),"unknown parameters rejected");
    bool changed=true;
    require(settings.set("thickness",2.25F,&changed)&&!changed,"reapplying stored settings preserves animation history");
    ProceduralWaveSampler smooth,unfiltered;
    ProceduralWaveSettings smoothSettings,unfilteredSettings;
    unfilteredSettings.reTemporalSmooth=0;
    smooth.update(36,smoothSettings); unfiltered.update(36,unfilteredSettings);
    smooth.update(36.01,smoothSettings); unfiltered.update(36.01,unfilteredSettings);
    require(smooth.frame().control!=unfiltered.frame().control,"temporal smoothing changes continuous motion");
    const auto held=smooth.frame().control;
    smooth.update(36.01,smoothSettings);
    require(smooth.frame().control==held,"frozen visual-capture timestamps hold the exact frame");
    settings.flowSpeed=-0.18F;
    a.update(100,settings);
    require(std::all_of(a.frame().control.begin(),a.frame().control.end(),[](float v){return std::isfinite(v);}),
            "reverse animation remains finite");
    a.update(std::numeric_limits<double>::infinity(),settings);
    require(std::all_of(a.frame().control.begin(),a.frame().control.end(),[](float v){return std::isfinite(v);}),
            "invalid external clock has a deterministic finite fallback");
    ProceduralParticleSettings particles;
    particles.count=100000;
    a.update(36,settings,particles);
    require(a.frame().settings[10][2]==4096,"particle upload bounds untrusted theme counts");
    static_assert(sizeof(ProceduralWaveFrame)==(16*4+64*28)*sizeof(float));
    std::cout<<"procedural wave contracts passed\n";
}
