// SPDX-License-Identifier: GPL-2.0-or-later
#include "remix_scene.h"
#include "remix_protocol.h"
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>

void Require(bool ok,const char* what) { if(!ok) throw std::runtime_error(what); }
int main() {
    using namespace BbRemix;
    SceneCamera camera{{1,0,0,10,0,1,0,20,0,0,1,30},{2,3,1,-0.1f}};
    const CapturedVertex vertices[]{
        {{-2,-3,2.9f,3},{0,0},1,0},{{2,-3,2.9f,3},{1,0},1,0},
        {{-2,3,2.9f,3},{0,1},1,0},{{2,3,2.9f,3},{1,1},1,0}};
    const uint32_t triangle[]{0,1,2};
    auto mesh=BuildMesh(vertices,triangle,Topology::Triangles,camera);
    Require(mesh.vertices.size()==3 && mesh.indices.size()==3,"perspective mesh");
    Require(mesh.vertices[0].position[0]==9 && mesh.vertices[0].position[1]==19 &&
            mesh.vertices[0].position[2]==33,"homogeneous clip reconstruction");
    Require(std::abs(mesh.vertices[0].normal[2]-1)<1e-6f,"normal reconstruction");
    auto moved=camera; moved.inverse_view[3]+=1;
    auto clips=std::vector<CapturedVertex>(std::begin(vertices),std::end(vertices));
    for(auto& v:clips) v.clip[0]-=2;
    Require(BuildMesh(clips,triangle,Topology::Triangles,moved).revision==mesh.revision,
            "static mesh survives camera movement");
    clips[0].written=0;
    Require(BuildMesh(clips,triangle,Topology::Triangles,moved).indices.empty(),"unwritten capture rejected");
    clips[0]=vertices[0]; clips[0].clip[0]=NAN;
    Require(BuildMesh(clips,triangle,Topology::Triangles,camera).indices.empty(),"NaN rejected");
    const uint32_t bad[]{0,1,99};
    Require(BuildMesh(vertices,bad,Topology::Triangles,camera).indices.empty(),"index bounds rejected");
    const uint32_t strip[]{0,1,2,3,UINT32_MAX,0,1,2};
    auto strip_mesh=BuildMesh(vertices,strip,Topology::Strip,camera);
    Require(strip_mesh.indices.size()==9,"strip restart and primitive count");
    for(const auto& v:strip_mesh.vertices) Require(v.normal[2]>0.999f,"consistent strip winding");
    auto repeated=std::vector<CapturedVertex>{vertices[0],vertices[1],vertices[2],vertices[2],vertices[1],vertices[3]};
    const uint32_t implicit[]{0,1,2,3,4,5};
    Require(BuildMesh(repeated,implicit,Topology::Triangles,camera).vertices.size()==4,"TF duplicates welded");
    SceneCamera invalid=camera; invalid.projection[0]=0;
    Require(BuildMesh(vertices,triangle,Topology::Triangles,invalid).indices.empty(),"invalid camera");
    Wire::Writer wire; Wire::Mesh packet{1,2,3,4,6}; wire.Put(packet);
    Wire::Reader reader(wire.data); Require(reader.Get<Wire::Mesh>().indices==6 && reader.End(),"IPC round trip");
    bool threw=false; try { reader.Get<Wire::Mesh>(); } catch(const std::runtime_error&) { threw=true; }
    Require(threw,"truncated IPC rejected");
    std::puts("PASS: Remix world reconstruction, camera-invariant caching, normals, bounds, strips, welding and IPC validation");
}
