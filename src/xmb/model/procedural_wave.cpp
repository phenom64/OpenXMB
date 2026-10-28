#include "openxmb/xmb/procedural_wave.hpp"

#include <algorithm>
#include <cmath>

namespace openxmb::xmb {
namespace {
constexpr float tau = 6.2831853071795864769F;
float fract(float v) noexcept { return v - std::floor(v); }
float mix(float a, float b, float t) noexcept { return a + (b - a) * t; }
float spline(const float* cp, float u) noexcept {
    const float s = std::clamp(u, 0.0F, 1.0F) * 25.0F;
    const auto i = std::min(static_cast<std::size_t>(s), std::size_t{24});
    const float t = std::min(s - static_cast<float>(i), 1.0F), t2 = t*t, t3 = t2*t;
    return ((1-3*t+3*t2-t3)*cp[i] + (4-6*t2+3*t3)*cp[i+1] +
            (1+3*t+3*t2-3*t3)*cp[i+2] + t3*cp[i+3]) / 6.0F;
}
}

bool ProceduralWaveSettings::set(std::string_view key, float value, bool* changed) noexcept {
    if(changed) *changed=false;
    if (!std::isfinite(value) || std::abs(value) > 10000.0F) return false;
#define WAVE_PARAMETER(name) if (key == #name) { if(changed) *changed=name!=value; name = value; return true; }
    WAVE_PARAMETER(flowSpeed) WAVE_PARAMETER(tension) WAVE_PARAMETER(damping)
    WAVE_PARAMETER(length) WAVE_PARAMETER(spacing) WAVE_PARAMETER(timeStep)
    WAVE_PARAMETER(bandAmplitude) WAVE_PARAMETER(bandSecondaryFreq) WAVE_PARAMETER(bandSecondaryAmp)
    WAVE_PARAMETER(travelSpeed1) WAVE_PARAMETER(travelAmp1) WAVE_PARAMETER(travelSpeed2) WAVE_PARAMETER(travelAmp2)
    WAVE_PARAMETER(perturbation) WAVE_PARAMETER(perturbationScale)
    WAVE_PARAMETER(waveCosAmp) WAVE_PARAMETER(waveBias) WAVE_PARAMETER(waveHeightScale) WAVE_PARAMETER(waveSoftClip)
    WAVE_PARAMETER(rePipelineBlend) WAVE_PARAMETER(reDescriptorStrength) WAVE_PARAMETER(reSyntheticDescriptorSeed)
    WAVE_PARAMETER(reSyntheticDescriptorMotion) WAVE_PARAMETER(reKernelGain) WAVE_PARAMETER(reNormalizeGain)
    WAVE_PARAMETER(reKernelPhaseStep) WAVE_PARAMETER(reIndexJitter) WAVE_PARAMETER(reTemporalSmooth)
    WAVE_PARAMETER(fresnelPower) WAVE_PARAMETER(fresnelScale) WAVE_PARAMETER(opacity) WAVE_PARAMETER(brightness) WAVE_PARAMETER(zDetailScale)
    WAVE_PARAMETER(ffdScale1X) WAVE_PARAMETER(ffdScale1Y) WAVE_PARAMETER(ffdScale1Z)
    WAVE_PARAMETER(ffdScale2X) WAVE_PARAMETER(ffdScale2Y) WAVE_PARAMETER(ffdScale2Z)
    WAVE_PARAMETER(ffdOffsetX) WAVE_PARAMETER(ffdOffsetY) WAVE_PARAMETER(ffdOffsetZ) WAVE_PARAMETER(ffdYAmp) WAVE_PARAMETER(ffdZAmp)
    WAVE_PARAMETER(posY) WAVE_PARAMETER(tilt) WAVE_PARAMETER(thickness)
    WAVE_PARAMETER(bandFrequency) WAVE_PARAMETER(rowSpread) WAVE_PARAMETER(cosFrequency) WAVE_PARAMETER(fill) WAVE_PARAMETER(edgeSoft)
#undef WAVE_PARAMETER
    return false;
}

void ProceduralWaveSampler::update(double seconds, const ProceduralWaveSettings& s,
                                    const ProceduralParticleSettings& p) noexcept {
    // Keep shader precision stable for long-running desktop sessions. The very
    // long wrap is outside the normal animation period and all storage is fixed.
    const float t = std::isfinite(seconds) ? static_cast<float>(std::fmod(std::max(0.0, seconds), 86400.0)) : 0.0F;
    if (s.reSyntheticDescriptorSeed != previous_seed_ || s.reDescriptorStrength != previous_strength_ ||
        s.length != previous_length_ || s.tension != previous_tension_ || s.perturbationScale != previous_scale_) {
        reset_history();
        for (std::size_t i=0; i<descriptor_.size(); ++i) {
            const float f=static_cast<float>(i)/descriptor_.size();
            const float wave=std::sin(f*97.0F*(0.3F+s.length))*0.5F +
                std::sin(f*211.0F*(0.15F+s.tension*3))*0.3F +
                std::sin(f*17.0F*(0.2F+s.perturbationScale*2))*0.2F;
            const float noise=fract(std::sin(static_cast<float>(i)*13.37F+s.reSyntheticDescriptorSeed*0.01F)*43758.5453123F)*2-1;
            descriptor_[i]=static_cast<std::uint8_t>(std::clamp((wave*s.reDescriptorStrength+noise*0.35F+1)*0.5F,0.0F,1.0F)*255);
        }
        previous_seed_=s.reSyntheticDescriptorSeed; previous_strength_=s.reDescriptorStrength;
        previous_length_=s.length; previous_tension_=s.tension; previous_scale_=s.perturbationScale;
    }
    constexpr std::array<float,4> normA{0.39584F,-0.0052389996F,-0.58664495F,0.189007F};
    constexpr std::array<float,4> normB{-0.003751F,-0.57536095F,0.161975F,0.417137F};
    const std::array<float,4> runtime{s.damping,s.length,s.tension,s.spacing*0.001F};
    const float phase=t*s.flowSpeed*s.timeStep*s.reSyntheticDescriptorMotion;
    for (std::size_t entry=0; entry<361; ++entry) {
        for (std::size_t lane=0; lane<4; ++lane) {
            float raw=0;
            for (std::size_t block=0; block<4; ++block) {
                const auto byte=(entry*0x130+block*0x10+lane*4+entry%7)%descriptor_.size();
                const float centered=descriptor_[byte]/255.0F*2-1;
                const float harmonic=std::sin(entry*0.07F+block*0.91F+lane*1.37F+phase*0.23F);
                raw+=(centered*0.75F+harmonic*0.25F)*runtime[block];
            }
            const float divisor=std::copysign(std::max(std::abs(normB[lane]),0.05F),normB[lane]);
            table_[entry*4+lane]=std::tanh((raw-normA[lane])/divisor*s.reNormalizeGain);
        }
    }
    for (std::size_t iteration=0; iteration<8; ++iteration) {
        const float ph=t*s.reKernelPhaseStep+iteration*0.37F;
        std::array<std::array<float,4>,4> vectors{};
        for (std::size_t lane=0; lane<4; ++lane) {
            const unsigned word=(lane*0x11+iteration*0x13)&0xff;
            const unsigned index=(19*(word>>4)+(word&15))%361;
            float fi=std::fmod(index+std::sin(ph+lane*0.77F)*s.reIndexJitter*361,361.0F);
            if (fi<0) fi+=361;
            const auto i0=static_cast<unsigned>(fi), i1=(i0+1)%361;
            for (std::size_t k=0;k<4;++k) vectors[lane][k]=mix(table_[i0*4+k],table_[i1*4+k],fract(fi));
        }
        const float a=0.5F+0.5F*std::sin(ph*0.7F), b=0.5F+0.5F*std::cos(ph*0.9F);
        for (std::size_t k=0;k<4;++k) {
            const std::array<float,8> values{
                mix(vectors[0][k],vectors[1][k],a),mix(vectors[1][k],vectors[2][k],b),
                mix(vectors[2][k],vectors[3][k],a),mix(vectors[3][k],vectors[0][k],b),
                vectors[1][k]-vectors[0][k],vectors[2][k]-vectors[1][k],
                vectors[3][k]-vectors[2][k],vectors[0][k]-vectors[3][k]};
            for (std::size_t j=0;j<8;++j) kernel_[(iteration*8+j)*4+k]=values[j];
        }
    }
    // Match the reference's low-pass kernel while preserving its rate on 30,
    // 60 and high-refresh displays. Frozen timestamps reuse the exact state;
    // seeking or a suspended app resumes from a fresh, deterministic sample.
    const double delta=static_cast<double>(t)-previous_seconds_;
    const float history=previous_seconds_>=0 && delta>=0 && delta<=0.25
        ? std::pow(std::clamp(s.reTemporalSmooth,0.0F,0.999F),static_cast<float>(delta*60.0)) : 0.0F;
    for(std::size_t i=0;i<kernel_.size();++i) {
        kernel_[i]=mix(kernel_[i],previous_kernel_[i],history);
        previous_kernel_[i]=kernel_[i];
    }
    previous_seconds_=t;
    const float flow=t*s.flowSpeed*s.timeStep;
    for (std::size_t row=0;row<kProceduralWaveRows;++row) {
        const float z=static_cast<float>(row)/(kProceduralWaveRows-1)*2-1;
        for (std::size_t i=0;i<kProceduralWaveControlPoints;++i) {
            const float x=static_cast<float>(i)/(kProceduralWaveControlPoints-1);
            const float kvf=row*0.93F+i*0.61F+flow*0.35F;
            float wrapped=std::fmod(std::floor(kvf),64.0F);
            if(wrapped<0) wrapped+=64.0F;
            const auto kv0=static_cast<unsigned>(wrapped),kv1=(kv0+1)%64;
            const float f=fract(kvf);
            const float core=(mix(kernel_[kv0*4],kernel_[kv1*4],f)*0.45F+
                mix(kernel_[kv0*4+1],kernel_[kv1*4+1],f)*0.25F+
                mix(kernel_[kv0*4+2],kernel_[kv1*4+2],f)*0.2F+
                mix(kernel_[kv0*4+3],kernel_[kv1*4+3],f)*0.1F)*s.reKernelGain+
                std::sin(flow*0.25F+z*s.rowSpread+x*s.bandFrequency)*s.bandAmplitude+
                std::cos(z*s.bandSecondaryFreq+x*4.8F+flow*0.09F)*s.bandSecondaryAmp;
            const float legacy=std::sin(x*tau*0.65F+z*0.8F-flow*s.travelSpeed1)*s.travelAmp1*s.tension+
                std::sin(x*tau*1.4F-z*1.2F+flow*s.travelSpeed2)*s.travelAmp2+
                s.perturbation*s.perturbationScale*std::sin((x*(4+s.length*2)+z*4-flow*0.6F)*s.spacing*0.01F);
            frame_.control[row*kProceduralWaveControlPoints+i]=mix(legacy,core,s.rePipelineBlend);
        }
    }
    frame_.settings[0]={s.flowSpeed,s.tension,s.damping,s.length};
    frame_.settings[1]={s.spacing,s.perturbation,s.perturbationScale,s.timeStep};
    frame_.settings[2]={s.waveCosAmp,s.waveBias,s.waveHeightScale,std::max(s.waveSoftClip,0.0001F)};
    frame_.settings[3]={s.ffdScale1X,s.ffdScale1Y,s.ffdScale1Z,s.ffdYAmp};
    frame_.settings[4]={s.ffdScale2X,s.ffdScale2Y,s.ffdScale2Z,s.ffdZAmp};
    frame_.settings[5]={s.ffdOffsetX,s.ffdOffsetY,s.ffdOffsetZ,s.zDetailScale};
    frame_.settings[6]={s.posY,s.tilt,s.thickness,s.cosFrequency};
    frame_.settings[7]={s.opacity,s.brightness,std::max(s.fresnelPower,0.001F),s.fresnelScale};
    frame_.settings[8]={s.fill,std::clamp(s.edgeSoft,0.0F,1.0F),t,0};
    frame_.settings[9]={p.opacity,p.sizeBase,p.sizeVar,p.flowSpeed};
    frame_.settings[10]={p.posY,p.spreadY,static_cast<float>(std::min(p.count,4096u)),0};
}

float ProceduralWaveSampler::displacement(float u,float v) const noexcept {
    if(!std::isfinite(u) || !std::isfinite(v)) return 0.0F;
    const float row=std::clamp(v,0.0F,1.0F)*(kProceduralWaveRows-1);
    const auto r0=std::min(static_cast<std::size_t>(row),kProceduralWaveRows-1),r1=std::min(r0+1,kProceduralWaveRows-1);
    return mix(spline(frame_.control.data()+r0*kProceduralWaveControlPoints,u),
               spline(frame_.control.data()+r1*kProceduralWaveControlPoints,u),row-static_cast<float>(r0));
}

} // namespace openxmb::xmb
