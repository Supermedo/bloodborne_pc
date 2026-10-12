// SPDX-License-Identifier: GPL-2.0-or-later
// Official SDK process serving actual game scenes. It contains no synthetic scene.
#include "remix_renderer.h"
#include "remix_protocol.h"
#include "../../src/crash_win.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

namespace {
LONG WINAPI DumpUnhandled(EXCEPTION_POINTERS* exception) {
    crash_win_dump(exception);
    return EXCEPTION_EXECUTE_HANDLER;
}
bool Transfer(HANDLE pipe, void* data, size_t bytes, bool write) {
    auto* p=static_cast<uint8_t*>(data);
    while (bytes) {
        DWORD done{};
        const DWORD n=DWORD(std::min<size_t>(bytes,1u<<20));
        if (!(write ? WriteFile(pipe,p,n,&done,nullptr) : ReadFile(pipe,p,n,&done,nullptr)) || !done) return false;
        p+=done; bytes-=done;
    }
    return true;
}
void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Update { BbRemix::Wire::Mesh info; std::wstring albedo, normal;
    std::vector<BbRemix::Vertex> vertices; std::vector<uint32_t> indices; };
}
int wmain(int argc, wchar_t** argv) {
    if (argc!=4 || std::wstring_view(argv[1]).find(L"\\\\.\\pipe\\bb-remix-")!=0) return 2;
    crash_win_init();
    SetUnhandledExceptionFilter(DumpUnhandled);
    // The SDK's D3D9 event flush must fence CopyRenderingOutput deterministically.
    SetEnvironmentVariableW(L"DXVK_EXPLICIT_FLUSH",L"1");
    // Start with the official importance-sampled path tracer. Avoid an additional
    // CUDA/NRC context while the game and SDK already own separate GPU devices.
    SetEnvironmentVariableW(L"RTX_INTEGRATE_INDIRECT_MODE",L"0");
    SetEnvironmentVariableW(L"RTX_DLSS_PRESET",L"2");
    SetEnvironmentVariableW(L"DXVK_RAY_RECONSTRUCTION",L"0");
    SetEnvironmentVariableW(L"DXVK_GRAPHICS_PRESET_TYPE",L"4");
    HANDLE pipe=CreateFileW(argv[1],GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
    if (pipe==INVALID_HANDLE_VALUE) return 3;
    BbRemix::Renderer renderer;
    std::unordered_set<uint64_t> mesh_ids;
    std::unordered_map<uint64_t,uint64_t> mesh_materials;
    std::unordered_set<uint64_t> material_ids;
    bool initialized=false, ready=false;
    uint64_t frame_count=0;
    auto clear=[&] {
        for (uint64_t id:mesh_ids) Require(renderer.DestroyMesh(id),renderer.Error().c_str());
        mesh_ids.clear(); mesh_materials.clear();
        for (uint64_t id:material_ids) Require(renderer.DestroyMaterial(id),renderer.Error().c_str());
        material_ids.clear();
    };
    try {
        for (;;) {
            BbRemix::Wire::Header header{};
            if (!Transfer(pipe,&header,sizeof(header),false)) break;
            Require(header.magic==BbRemix::Wire::Magic && header.version==BbRemix::Wire::Version &&
                    header.bytes<=BbRemix::Wire::MaxMessage,"Invalid Remix IPC header");
            std::vector<uint8_t> bytes(header.bytes);
            Require(Transfer(pipe,bytes.data(),bytes.size(),false),"Remix IPC disconnected");
            BbRemix::Wire::Reader input(bytes);
            BbRemix::Wire::Reply reply{};
            if (header.operation==BbRemix::Wire::Operation::Stop) break;
            if (header.operation==BbRemix::Wire::Operation::Initialize) {
                auto info=input.Get<BbRemix::Wire::Initialize>();
                Require(!initialized && input.End(),"Duplicate Remix initialization");
                Require(renderer.Initialize(std::filesystem::absolute(argv[2]),info.width,info.height,info.luid),renderer.Error().c_str());
                Require(renderer.Config("rtx.graphicsPreset","4"),renderer.Error().c_str());
                Require(renderer.Config("rtx.integrateIndirectMode","0"),renderer.Error().c_str());
                // One owner for reconstruction. Game DLSS/FSR/FG is bypassed in Remix mode.
                Require(renderer.Config("rtx.dlfg.enable","False"),renderer.Error().c_str());
                initialized=true;
                // Until native analytical-light extraction is complete, SDK environment
                // lighting keeps captured PBR surfaces visible. It is deliberately explicit.
                remixapi_LightInfoDomeEXT dome{};
                dome.sType=REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT;
                dome.transform={{{1,0,0,0},{0,1,0,0},{0,0,1,0}}};
                // The runtime's dome path needs a real texture even for uniform
                // lighting. An empty TextureRef reaches a null descriptor later.
                const std::filesystem::path environment=std::filesystem::absolute(argv[3]);
                std::filesystem::create_directories(environment.parent_path());
                uint32_t dds[37]{}; dds[0]=0x20534444; dds[1]=124; dds[2]=0x100f;
                dds[3]=dds[4]=1; dds[5]=8; dds[7]=1; dds[19]=32; dds[20]=4;
                dds[21]=0x30315844; dds[27]=0x1000; dds[32]=10; dds[33]=3; dds[35]=1;
                const uint16_t white[]{0x3c00,0x3c00,0x3c00,0x3c00};
                { std::ofstream file(environment,std::ios::binary);
                  file.write(reinterpret_cast<const char*>(dds),sizeof(dds));
                  file.write(reinterpret_cast<const char*>(white),sizeof(white)); Require(bool(file),"SDK environment texture write failed"); }
                const std::wstring environment_path=environment.wstring();
                dome.colorTexture=environment_path.c_str();
                remixapi_LightInfo light{REMIXAPI_STRUCT_TYPE_LIGHT_INFO,&dome,0x42424c49474854,{0.35f,0.4f,0.5f}};
                Require(renderer.Light(light,1),renderer.Error().c_str());
                reply.success=1;
            } else if (header.operation==BbRemix::Wire::Operation::Frame) {
                Require(initialized,"Remix frame before initialization");
                auto frame=input.Get<BbRemix::Wire::Frame>();
                Require(frame.camera.Valid() && frame.updates<=BbRemix::Wire::MaxMeshes &&
                        frame.instances<=BbRemix::Wire::MaxMeshes,"Invalid Remix frame");
                std::vector<Update> updates;
                updates.reserve(frame.updates);
                uint64_t total_vertices=0;
                for (uint32_t n=0;n<frame.updates;++n) {
                    Update u; u.info=input.Get<BbRemix::Wire::Mesh>();
                    const auto& m=u.info;
                    Require(m.id && m.material && m.vertices && m.vertices<=BbRemix::Wire::MaxVertices &&
                            m.indices && m.indices<=(8u<<20) && m.indices%3==0,"Invalid Remix mesh packet");
                    total_vertices+=m.vertices;
                    Require(total_vertices<=(2u<<20),"Remix frame mesh update budget exceeded");
                    u.albedo=input.Path(m.albedo_chars); u.normal=input.Path(m.normal_chars);
                    if (!u.albedo.empty()) Require(std::filesystem::path(u.albedo).is_absolute(),"Relative albedo path");
                    if (!u.normal.empty()) Require(std::filesystem::path(u.normal).is_absolute(),"Relative normal path");
                    u.vertices.resize(m.vertices); u.indices.resize(m.indices);
                    input.Bytes(u.vertices.data(),u.vertices.size()*sizeof(BbRemix::Vertex));
                    input.Bytes(u.indices.data(),u.indices.size()*sizeof(uint32_t));
                    for (uint32_t i:u.indices) Require(i<m.vertices,"Remix index outside mesh");
                    for (const auto& v:u.vertices) {
                        for (float f:v.position) Require(std::isfinite(f),"Non-finite Remix position");
                        for (float f:v.normal) Require(std::isfinite(f),"Non-finite Remix normal");
                        for (float f:v.uv) Require(std::isfinite(f),"Non-finite Remix UV");
                    }
                    updates.push_back(std::move(u));
                }
                std::vector<uint64_t> instances(frame.instances);
                input.Bytes(instances.data(),instances.size()*sizeof(uint64_t));
                Require(input.End(),"Trailing Remix frame data");
                if (frame.reset) clear();
                Require(renderer.Resize(frame.width,frame.height),renderer.Error().c_str());
                for (auto& u:updates) {
                    const auto& m=u.info;
                    if (!material_ids.contains(m.material)) {
                        auto opaque=BbRemix::Renderer::OpaqueDefaults();
                        opaque.alphaTestType=m.alpha_cutout ? 6 : 7;
                        opaque.alphaReferenceValue=128;
                        auto material=BbRemix::Renderer::MaterialDefaults(m.material);
                        material.pNext=&opaque;
                        material.albedoTexture=u.albedo.empty() ? nullptr : u.albedo.c_str();
                        material.normalTexture=u.normal.empty() ? nullptr : u.normal.c_str();
                        Require(renderer.Material(material,1),renderer.Error().c_str());
                        material_ids.insert(m.material);
                    }
                    std::vector<remixapi_HardcodedVertex> vertices(u.vertices.size());
                    for (size_t i=0;i<vertices.size();++i) {
                        std::copy_n(u.vertices[i].position,3,vertices[i].position);
                        std::copy_n(u.vertices[i].normal,3,vertices[i].normal);
                        std::copy_n(u.vertices[i].uv,2,vertices[i].texcoord);
                        vertices[i].color=0xffffffff;
                    }
                    remixapi_MeshInfoSurfaceTriangles surface{};
                    surface.vertices_values=vertices.data(); surface.vertices_count=vertices.size();
                    surface.indices_values=u.indices.data(); surface.indices_count=u.indices.size();
                    surface.material=renderer.MaterialHandle(m.material);
                    remixapi_MeshInfo mesh{REMIXAPI_STRUCT_TYPE_MESH_INFO,nullptr,m.id,&surface,1};
                    Require(renderer.Mesh(mesh,m.revision),renderer.Error().c_str());
                    mesh_ids.insert(m.id); mesh_materials[m.id]=m.material;
                }
                const std::unordered_set<uint64_t> wanted(instances.begin(),instances.end());
                for (auto it=mesh_ids.begin();it!=mesh_ids.end();) {
                    if (!wanted.contains(*it)) {
                        Require(renderer.DestroyMesh(*it),renderer.Error().c_str());
                        mesh_materials.erase(*it); it=mesh_ids.erase(it);
                    } else ++it;
                }
                for (auto it=material_ids.begin();it!=material_ids.end();) {
                    if (std::none_of(mesh_materials.begin(),mesh_materials.end(),[&](const auto& m){ return m.second==*it; })) {
                        Require(renderer.DestroyMaterial(*it),renderer.Error().c_str()); it=material_ids.erase(it);
                    } else ++it;
                }
                const auto& c=frame.camera;
                remixapi_CameraInfoParameterizedEXT cp{};
                cp.sType=REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
                cp.position={c.inverse_view[3],c.inverse_view[7],c.inverse_view[11]};
                const float sx=std::copysign(1.f,c.projection[0]*c.viewport_sign[0]);
                const float sy=-std::copysign(1.f,c.projection[1]*c.viewport_sign[1]);
                cp.right={c.inverse_view[0]*sx,c.inverse_view[4]*sx,c.inverse_view[8]*sx};
                cp.up={c.inverse_view[1]*sy,c.inverse_view[5]*sy,c.inverse_view[9]*sy};
                cp.forward={c.inverse_view[2],c.inverse_view[6],c.inverse_view[10]};
                cp.fovYInDegrees=2.f*std::atan(1.f/std::abs(c.projection[1]))*180.f/3.14159265358979323846f;
                cp.aspect=std::abs(c.projection[1]/c.projection[0]);
                cp.nearPlane=std::clamp(std::abs(c.projection[3]/c.projection[2]),0.001f,100.f);
                cp.farPlane=3000;
                remixapi_CameraInfo camera{REMIXAPI_STRUCT_TYPE_CAMERA_INFO,&cp};
                Require(renderer.Camera(camera),renderer.Error().c_str());
                remixapi_InstanceInfo instance{};
                instance.sType=REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
                instance.transform={{{1,0,0,0},{0,1,0,0},{0,0,1,0}}}; instance.doubleSided=true;
                for (uint64_t id:instances) Require(renderer.Draw(id,instance),renderer.Error().c_str());
                Require(renderer.DrawLight(0x42424c49474854),renderer.Error().c_str());
                Require(renderer.Render(),renderer.Error().c_str());
                if (!ready) {
                    std::vector<uint16_t> pixels(size_t(frame.width)*frame.height*4);
                    Require(renderer.Readback(pixels),renderer.Error().c_str());
                    for (size_t i=0;i<pixels.size();i+=4)
                        if ((pixels[i]&0x7fff)>0x0400 && (pixels[i]&0x7c00)!=0x7c00) { ready=true; break; }
                }
                auto output=renderer.Output();
                reply.success=1; reply.ready=ready;
                reply.memory=reinterpret_cast<uint64_t>(output.memory); reply.generation=output.generation;
                reply.width=output.width; reply.height=output.height; reply.luid=output.adapter_luid;
                if (++frame_count%300==1) std::printf("Remix game scene: frame %llu, %zu meshes, %u updates, %s\n",
                    static_cast<unsigned long long>(frame.number),mesh_ids.size(),frame.updates,ready ? "rendered" : "shader warmup");
                std::fflush(stdout);
            } else throw std::runtime_error("Unknown Remix operation");
            Require(Transfer(pipe,&reply,sizeof(reply),true),"Remix IPC reply disconnected");
        }
        renderer.Shutdown(); CloseHandle(pipe); return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr,"Remix game host failed: %s\n",e.what()); std::fflush(stderr);
        BbRemix::Wire::Reply failed{}; Transfer(pipe,&failed,sizeof(failed),true);
        renderer.Shutdown(); CloseHandle(pipe); return 1;
    }
}
