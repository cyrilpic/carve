// Begin License:
// Copyright (C) 2006-2008 Tobias Sargeant (tobias.sargeant@gmail.com).
// All rights reserved.
//
// This file is part of the Carve CSG Library (http://carve-csg.com/)
//
// This file may be used under the terms of the GNU General Public
// License version 2.0 as published by the Free Software Foundation
// and appearing in the file LICENSE.GPL2 included in the packaging of
// this file.
//
// This file is provided "AS IS" with NO WARRANTY OF ANY KIND,
// INCLUDING THE WARRANTIES OF DESIGN, MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE.
// End:


#if defined(HAVE_CONFIG_H)
#  include <carve_config.h>
#endif

#include <carve/csg.hpp>
#include <carve/polyline.hpp>
#include <carve/debug_hooks.hpp>
#include <carve/timing.hpp>
#include <carve/triangulator.hpp>

#include <list>
#include <set>
#include <iostream>

#include <algorithm>

#include "csg_detail.hpp"
#include "csg_data.hpp"

#include "intersect_common.hpp"

typedef carve::poly::Polyhedron poly_t;



#if defined(CARVE_DEBUG_WRITE_PLY_DATA)
void writePLY(std::string &out_file, const carve::line::PolylineSet *lines, bool ascii);
#endif



namespace {



  struct GraphEdge {
    GraphEdge *next;
    GraphEdge *prev;
    GraphEdge *loop_next;
    const poly_t::vertex_t *src;
    const poly_t::vertex_t *tgt;
    double ang;
    int visited;

    GraphEdge(const poly_t::vertex_t *_src, const poly_t::vertex_t *_tgt) :
      next(NULL), prev(NULL), loop_next(NULL),
      src(_src), tgt(_tgt),
      ang(0.0), visited(-1) {
    }
  };



  struct GraphEdges {
    GraphEdge *edges;
    carve::geom2d::P2 proj;

    GraphEdges() : edges(NULL), proj() {
    }
  };



  struct Graph {
    typedef std::unordered_map<const poly_t::vertex_t *, GraphEdges, carve::poly::hash_vertex_ptr> graph_t;

    graph_t graph;

    Graph() : graph() {
    }

    ~Graph() {
      int c = 0;

      GraphEdge *edge;
      for (graph_t::iterator i = graph.begin(), e =  graph.end(); i != e; ++i) {
        edge = (*i).second.edges;
        while (edge) {
          GraphEdge *temp = edge;
          ++c;
          edge = edge->next;
          delete temp;
        }
      }

      if (c) {
        std::cerr << "warning: "
                  << c
                  << " edges should have already been removed at graph destruction time"
                  << std::endl;
      }
    }

    const carve::geom2d::P2 &projection(const poly_t::vertex_t *v) const {
      graph_t::const_iterator i = graph.find(v);
      CARVE_ASSERT(i != graph.end());
      return (*i).second.proj;
    }

    void computeProjection(const poly_t::face_t *face) {
      for (graph_t::iterator i = graph.begin(), e =  graph.end(); i != e; ++i) {
        (*i).second.proj = carve::poly::face::project(face, (*i).first->v);
      }
      for (graph_t::iterator i = graph.begin(), e =  graph.end(); i != e; ++i) {
        for (GraphEdge *e = (*i).second.edges; e; e = e->next) {
          e->ang = carve::math::ANG(carve::geom2d::atan2(projection(e->tgt) - projection(e->src)));
        }
      }
    }

    void print(std::ostream &out, const carve::csg::VertexIntersections *vi) const {
      for (graph_t::const_iterator i = graph.begin(), e =  graph.end(); i != e; ++i) {
        out << (*i).first << (*i).first->v << '(' << projection((*i).first).x << ',' << projection((*i).first).y << ") :";
        for (const GraphEdge *e = (*i).second.edges; e; e = e->next) {
          out << ' ' << e->tgt << e->tgt->v << '(' << projection(e->tgt).x << ',' << projection(e->tgt).y << ')';
        }
        out << std::endl;
        if (vi) {
          carve::csg::VertexIntersections::const_iterator j = vi->find((*i).first);
          if (j != vi->end()) {
            out << "   (int) ";
            for (carve::csg::IObjPairSet::const_iterator
                   k = (*j).second.begin(), ke = (*j).second.end(); k != ke; ++k) {
              if ((*k).first < (*k).second) {
                out << (*k).first << ".." << (*k).second << "; ";
              }
            }
            out << std::endl;
          }
        }
      }
    }

    void addEdge(const poly_t::vertex_t *v1, const poly_t::vertex_t *v2) {
      GraphEdges &edges = graph[v1];
      GraphEdge *edge = new GraphEdge(v1, v2);
      if (edges.edges) edges.edges->prev = edge;
      edge->next = edges.edges;
      edges.edges = edge;
    }

    void removeEdge(GraphEdge *edge) {
      if (edge->prev != NULL) {
        edge->prev->next = edge->next;
      } else {
        if (edge->next != NULL) {
          GraphEdges &edges = (graph[edge->src]);
          edges.edges = edge->next;
        } else {
          graph.erase(edge->src);
        }
      }
      if (edge->next != NULL) {
        edge->next->prev = edge->prev;
      }
      delete edge;
    }

    bool empty() const {
      return graph.size() == 0;
    }

    GraphEdge *pickStartEdge() {
      // Try and find a vertex from which there is only one outbound edge. Won't always succeed.
      for (graph_t::iterator i = graph.begin(); i != graph.end(); ++i) {
        GraphEdges &ge = i->second;
        if (ge.edges->next == NULL) {
          return ge.edges;
        }
      }
      return (*graph.begin()).second.edges;
    }

    GraphEdge *outboundEdges(const poly_t::vertex_t *v) {
      return graph[v].edges;
    }
  };



  /** 
   * \brief Take a set of new edges and split a face based upon those edges.
   * 
   * @param[in] face The face to be split.
   * @param[in] edges 
   * @param[out] face_loops Output list of face loops
   * @param[out] hole_loops Output list of hole loops
   * @param vi 
   */
  static void splitFace(const poly_t::face_t *face,
                        const carve::csg::V2Set &edges,
                        std::list<std::vector<const poly_t::vertex_t *> > &face_loops,
                        std::list<std::vector<const poly_t::vertex_t *> > &hole_loops,
                        const carve::csg::VertexIntersections &vi) {
    Graph graph;

#if defined(CARVE_DEBUG)
    std::cerr << "splitFace()" << " face=" << face << " face->vertices.size()=" << face->vertices.size() << " edges.size()=" << edges.size() << std::endl;
#endif

    for (carve::csg::V2Set::const_iterator
           i = edges.begin(), e = edges.end();
         i != e;
         ++i) {
      const poly_t::vertex_t *v1 = ((*i).first), *v2 = ((*i).second);
      if (carve::geom::equal(v1->v, v2->v)) std::cerr << "WARNING! " << v1->v << "==" << v2->v << std::endl;
      graph.addEdge(v1, v2);
    }

    graph.computeProjection(face);
#if defined(CARVE_DEBUG)
    graph.print(std::cerr, &vi);
#endif

    while (!graph.empty()) {
      GraphEdge *edge;
      GraphEdge *start;
      start = edge = graph.pickStartEdge();

      edge->visited = 0;

      int len = 0;

      while (1) {
        double in_ang = M_PI + edge->ang;
        if (in_ang > M_TWOPI) in_ang -= M_TWOPI;

        GraphEdge *opts;
        GraphEdge *out = NULL;
        double best = M_TWOPI + 1.0;

        for (opts = graph.outboundEdges(edge->tgt); opts; opts = opts->next) {
          if (opts->tgt == edge->src) {
            if (out == NULL && opts->next == NULL) out = opts;
          } else {
            double out_ang = carve::math::ANG(in_ang - opts->ang);

            if (out == NULL || out_ang < best) {
              out = opts;
              best = out_ang;
            }
          }
        }

        CARVE_ASSERT(out != NULL);

        edge->loop_next = out;

        if (out->visited >= 0) {
          while (start != out) {
            GraphEdge *e = start;
            start = start->loop_next;
            e->loop_next = NULL;
            e->visited = -1;
          }
          len = edge->visited - out->visited + 1;
          break;
        }

        out->visited = edge->visited + 1;
        edge = out;
      }

      std::vector<const poly_t::vertex_t *> loop(len);
      std::vector<carve::geom2d::P2> projected(len);

      edge = start;
      for (int i = 0; i < len; ++i) {
        GraphEdge *next = edge->loop_next;
        loop[i] = edge->src;
        projected[i] = graph.projection(edge->src);
        graph.removeEdge(edge);
        edge = next;
      }
#if defined(CARVE_DEBUG)
      std::cerr << "===============================================" << std::endl;
      graph.print(std::cerr, &vi);
#endif
#if defined(CARVE_DEBUG)
      std::cerr << "signed area of loop: " << carve::geom2d::signedArea(projected) << std::endl;
#endif
      CARVE_ASSERT(edge == start);
      if (carve::geom2d::signedArea(projected) < 0) {
#if defined(CARVE_DEBUG)
        std::cerr << "output face loop size: " << loop.size() << " : ";
        for (size_t i = 0; i < loop.size(); ++i) std::cerr << " " << loop[i];
        std::cerr << std::endl;
#endif
        face_loops.push_back(std::vector<const poly_t::vertex_t *>());
        face_loops.back().swap(loop);
      } else {
#if defined(CARVE_DEBUG)
        std::cerr << "output hole loop size: " << loop.size() << " : ";
        for (size_t i = 0; i < loop.size(); ++i) std::cerr << " " << loop[i];
        std::cerr << std::endl;
#endif
        hole_loops.push_back(std::vector<const poly_t::vertex_t *>());
        hole_loops.back().swap(loop);
      }
    }
#if defined(CARVE_DEBUG)
    std::cerr << "===============================================" << std::endl;

    std::cerr << "result: " << face_loops.size() << " face loops (";
    for (std::list<std::vector<const poly_t::vertex_t *> >::const_iterator i = face_loops.begin(); i != face_loops.end(); ++i) {
      std::cerr << ((i != face_loops.begin()) ? " " : "") << (*i).size();
      for (unsigned j = 0; j < (*i).size(); ++j) {
        if (std::find((*i).begin() + j + 1, (*i).end(), (*i)[j]) != (*i).end()) {
          std::cerr << "[!]";
          break;
        }
      }
    }
    std::cerr << ") " << hole_loops.size() << " hole loops (";
    for (std::list<std::vector<const poly_t::vertex_t *> >::const_iterator i = hole_loops.begin(); i != hole_loops.end(); ++i) {
      std::cerr << ((i != hole_loops.begin()) ? " " : "") << (*i).size();
      for (unsigned j = 0; j < (*i).size(); ++j) {
        if (std::find((*i).begin() + j + 1, (*i).end(), (*i)[j]) != (*i).end()) {
          std::cerr << "[!]";
          break;
        }
      }
    }
    std::cerr << ")" << std::endl;
#endif
  }



  /** 
   * \brief Determine the relationship between a face loop and a hole loop.
   * 
   * Determine whether a face and hole share an edge, or a vertex,
   * or do not touch. Find a hole vertex that is not part of the
   * face, and a hole,face vertex pair that are coincident, if such
   * a pair exists.
   *
   * @param[in] f A face loop.
   * @param[in] f_sort A vector indexing \a f in address order
   * @param[in] h A hole loop.
   * @param[in] h_sort A vector indexing \a h in address order
   * @param[out] f_idx Index of a face vertex that is shared with the hole.
   * @param[out] h_idx Index of the hole vertex corresponding to \a f_idx.
   * @param[out] unmatched_h_idx Index of a hole vertex that is not part of the face.
   * @param[out] shares_vertex Boolean indicating that the face and the hole share a vertex.
   * @param[out] shares_edge Boolean indicating that the face and the hole share an edge.
   */
  static void compareFaceLoopAndHoleLoop(const std::vector<const poly_t::vertex_t *> &f,
                                         const std::vector<unsigned> &f_sort,
                                         const std::vector<const poly_t::vertex_t *> &h,
                                         const std::vector<unsigned> &h_sort,
                                         unsigned &f_idx,
                                         unsigned &h_idx,
                                         int &unmatched_h_idx,
                                         bool &shares_vertex,
                                         bool &shares_edge) {
    const size_t F = f.size();
    const size_t H = h.size();

    shares_vertex = shares_edge = false;
    unmatched_h_idx = -1;

    unsigned I, J;
    for (I = J = 0; I < F && J < H;) {
      unsigned i = f_sort[I], j = h_sort[J];
      if (f[i] == h[j]) {
        shares_vertex = true;
        f_idx = i;
        h_idx = j;
        if (f[(i + F - 1) % F] == h[(j + 1) % H]) {
          shares_edge = true;
        }
        const poly_t::vertex_t *t = f[i];
        do { ++I; } while (I < F && f[f_sort[I]] == t);
        do { ++J; } while (J < H && h[h_sort[J]] == t);
      } else if (f[i] < h[j]) {
        ++I;
      } else {
        unmatched_h_idx = j;
        ++J;
      }
    }
    if (J < H) {
      unmatched_h_idx = h_sort[J];
    }
  }



  /** 
   * \brief Compute an embedding for a set of face loops and hole loops.
   *
   * Because face and hole loops may be contained within each other,
   * it must be determined which hole loops are directly contained
   * within a face loop.
   * 
   * @param[in] face The face from which these face and hole loops derive.
   * @param[in] face_loops 
   * @param[in] hole_loops 
   * @param[out] containing_faces     A vector which for each hole loop
   *                                  lists the indices of the face
   *                                  loops it is containined in.
   * @param[out] hole_shared_vertices A map from a face,hole pair to
   *                                  a shared vertex pair.
   */
  static void computeContainment(const poly_t::face_t *face,
                                 std::vector<std::vector<const poly_t::vertex_t *> > &face_loops,
                                 std::vector<std::vector<const poly_t::vertex_t *> > &hole_loops,
                                 std::vector<std::vector<int> > &containing_faces,
                                 std::map<int, std::map<int, std::pair<unsigned, unsigned> > > &hole_shared_vertices) {
    std::vector<std::vector<carve::geom2d::P2> > face_loops_projected, hole_loops_projected;
    std::vector<std::vector<unsigned> > face_loops_sorted, hole_loops_sorted;

    std::vector<double> face_loop_areas, hole_loop_areas;

    face_loops_projected.resize(face_loops.size());
    face_loops_sorted.resize(face_loops.size());
    face_loop_areas.resize(face_loops.size());

    hole_loops.resize(hole_loops.size());
    hole_loops_projected.resize(hole_loops.size());
    hole_loops_sorted.resize(hole_loops.size());
    hole_loop_areas.resize(hole_loops.size());

    // produce a projection of each face loop onto a 2D plane, and a
    // index vector which sorts vertices by address.
    for (size_t m = 0; m < face_loops.size(); ++m) {
      const std::vector<const poly_t::vertex_t *> &f_loop = (face_loops[m]);
      face_loops_projected[m].reserve(f_loop.size());
      face_loops_sorted[m].reserve(f_loop.size());
      for (size_t n = 0; n < f_loop.size(); ++n) {
        face_loops_projected[m].push_back(carve::poly::face::project(face, f_loop[n]->v));
        face_loops_sorted[m].push_back(n);
      }
      face_loop_areas.push_back(carve::geom2d::signedArea(face_loops_projected[m]));
      std::sort(face_loops_sorted[m].begin(), face_loops_sorted[m].end(), 
                carve::index_sort<std::vector<const poly_t::vertex_t *> >(face_loops[m]));
    }

    // produce a projection of each hole loop onto a 2D plane, and a
    // index vector which sorts vertices by address.
    for (size_t m = 0; m < hole_loops.size(); ++m) {
      const std::vector<const poly_t::vertex_t *> &h_loop = (hole_loops[m]);
      hole_loops_projected[m].reserve(h_loop.size());
      hole_loops_projected[m].reserve(h_loop.size());
      for (size_t n = 0; n < h_loop.size(); ++n) {
        hole_loops_projected[m].push_back(carve::poly::face::project(face, h_loop[n]->v));
        hole_loops_sorted[m].push_back(n);
      }
      hole_loop_areas.push_back(carve::geom2d::signedArea(hole_loops_projected[m]));
      std::sort(hole_loops_sorted[m].begin(), hole_loops_sorted[m].end(), 
                carve::index_sort<std::vector<const poly_t::vertex_t *> >(hole_loops[m]));
    }

    containing_faces.resize(hole_loops.size());

    for (unsigned i = 0; i < hole_loops.size(); ++i) {

      for (unsigned j = 0; j < face_loops.size(); ++j) {
        unsigned f_idx, h_idx;
        int unmatched_h_idx;
        bool shares_vertex, shares_edge;
        compareFaceLoopAndHoleLoop(face_loops[j],
                                   face_loops_sorted[j],
                                   hole_loops[i],
                                   hole_loops_sorted[i],
                                   f_idx, h_idx,
                                   unmatched_h_idx,
                                   shares_vertex,
                                   shares_edge);

#if defined(CARVE_DEBUG)
        std::cerr << "face: " << j
                  << " hole: " << i
                  << " shares_vertex: " << shares_vertex
                  << " shares_edge: " << shares_edge
                  << std::endl;
#endif

        carve::geom3d::Vector test = hole_loops[i][0]->v;
        carve::geom2d::P2 test_p = carve::poly::face::project(face, test);

        if (shares_vertex) {
          hole_shared_vertices[i][j] = std::make_pair(h_idx, f_idx);
          // Hole touches face. Should be able to connect it up
          // trivially. Still need to record its containment, so that
          // the assignment below works.
          if (unmatched_h_idx != -1) {
#if defined(CARVE_DEBUG)
            std::cerr << "using unmatched vertex: " << unmatched_h_idx << std::endl;
#endif
            test = hole_loops[i][unmatched_h_idx]->v;
            test_p = carve::poly::face::project(face, test);
          } else {
            // XXX: hole shares ALL vertices with face. Pick a point
            // internal to the projected poly.
            if (shares_edge) {
              // Hole shares edge with face => face can't contain hole.
              continue;
            }

            // XXX: how is this possible? Doesn't share an edge, but
            // also doesn't have any vertices that are not in
            // common. Degenerate hole?

            // XXX: come up with a test case for this.
            CARVE_FAIL("implement me");
          }
        }


        // XXX: use loop area to avoid some point-in-poly tests? Loop
        // area is faster, but not sure which is more robust.
        if (carve::geom2d::pointInPolySimple(face_loops_projected[j], test_p)) {
#if defined(CARVE_DEBUG)
          std::cerr << "contains: " << i << " - " << j << std::endl;
#endif
          containing_faces[i].push_back(j);
        } else {
#if defined(CARVE_DEBUG)
          std::cerr << "does not contain: " << i << " - " << j << std::endl;
#endif
        }
      }

#if defined(CARVE_DEBUG)
      if (containing_faces[i].size() == 0) {
        //HOOK(drawFaceLoopWireframe(hole_loops[i], face->normal, 1.0, 0.0, 0.0, 1.0););
        std::cerr << "hole loop: ";
        for (unsigned j = 0; j < hole_loops[i].size(); ++j) {
          std::cerr << " " << hole_loops[i][j] << ":" << hole_loops[i][j]->v;
        }
        std::cerr << std::endl;
        for (unsigned j = 0; j < face_loops.size(); ++j) {
          //HOOK(drawFaceLoopWireframe(face_loops[j], face->normal, 0.0, 1.0, 0.0, 1.0););
        }
      }
#endif

      // CARVE_ASSERT(containing_faces[i].size() >= 1);
    }
  }



  /** 
   * \brief Merge face loops and hole loops to produce a set of face loops without holes.
   * 
   * @param[in] face The face from which these face loops derive.
   * @param[in,out] f_loops A list of face loops.
   * @param[in] h_loops A list of hole loops to be incorporated into face loops.
   */
  static void mergeFacesAndHoles(const poly_t::face_t *face,
                                 std::list<std::vector<const poly_t::vertex_t *> > &f_loops,
                                 std::list<std::vector<const poly_t::vertex_t *> > &h_loops,
                                 carve::csg::CSG::Hooks &hooks) {
    std::vector<std::vector<const poly_t::vertex_t *> > face_loops;
    std::vector<std::vector<const poly_t::vertex_t *> > hole_loops;

    std::vector<std::vector<int> > containing_faces;
    std::map<int, std::map<int, std::pair<unsigned, unsigned> > > hole_shared_vertices;

    {
      // move input face and hole loops to temp vectors.
      size_t m;
      face_loops.resize(f_loops.size());
      m = 0;
      for (std::list<std::vector<const poly_t::vertex_t *> >::iterator
             i = f_loops.begin(), ie = f_loops.end();
           i != ie;
           ++i, ++m) {
        face_loops[m].swap((*i));
      }

      hole_loops.resize(h_loops.size());
      m = 0;
      for (std::list<std::vector<const poly_t::vertex_t *> >::iterator
             i = h_loops.begin(), ie = h_loops.end();
           i != ie;
           ++i, ++m) {
        hole_loops[m].swap((*i));
      }
      f_loops.clear();
      h_loops.clear();
    }

    // work out the embedding of holes and faces.
    computeContainment(face, face_loops, hole_loops, containing_faces, hole_shared_vertices);

    int unassigned = (int)hole_loops.size();

    std::vector<std::vector<int> > face_holes;
    face_holes.resize(face_loops.size());

    for (unsigned i = 0; i < containing_faces.size(); ++i) {
      if (containing_faces[i].size() == 0) {
        std::map<int, std::map<int, std::pair<unsigned, unsigned> > >::iterator it = hole_shared_vertices.find(i);
        if (it != hole_shared_vertices.end()) {
          std::map<int, std::pair<unsigned, unsigned> >::iterator it2 = (*it).second.begin();
          int f = (*it2).first;
          unsigned h_idx = (*it2).second.first;
          unsigned f_idx = (*it2).second.second;

          // patch the hole into the face directly. because
          // f_loop[f_idx] == h_loop[h_idx], we don't need to
          // duplicate the f_loop vertex.

          std::vector<const poly_t::vertex_t *> &f_loop = face_loops[f];
          std::vector<const poly_t::vertex_t *> &h_loop = hole_loops[i];

          f_loop.insert(f_loop.begin() + f_idx + 1, h_loop.size(), NULL);

          unsigned p = f_idx + 1;
          for (unsigned a = h_idx + 1; a < h_loop.size(); ++a, ++p) {
            f_loop[p] = h_loop[a];
          }
          for (unsigned a = 0; a <= h_idx; ++a, ++p) {
            f_loop[p] = h_loop[a];
          }

#if defined(CARVE_DEBUG)
          std::cerr << "hook face " << f << " to hole " << i << "(vertex)" << std::endl;
#endif
        } else {
          std::cerr << "uncontained hole loop does not share vertices with any face loop!" << std::endl;
        }
        unassigned--;
      }
    }


    // work out which holes are directly contained within which faces.
    while (unassigned) {
      std::set<int> removed;

      for (unsigned i = 0; i < containing_faces.size(); ++i) {
        if (containing_faces[i].size() == 1) {
          int f = containing_faces[i][0];
          face_holes[f].push_back(i);
#if defined(CARVE_DEBUG)
          std::cerr << "hook face " << f << " to hole " << i << std::endl;
#endif
          removed.insert(f);
          unassigned--;
        }
      }
      for (std::set<int>::iterator f = removed.begin(); f != removed.end(); ++f) {
        for (unsigned i = 0; i < containing_faces.size(); ++i) {
          containing_faces[i].erase(std::remove(containing_faces[i].begin(),
                                                containing_faces[i].end(),
                                                *f),
                                    containing_faces[i].end());
        }
      }
    }

#if 0
    // use old templated projection code to patch holes into faces.
    for (unsigned i = 0; i < face_loops.size(); ++i) {
      std::vector<std::vector<const poly_t::vertex_t *> > face_hole_loops;
      face_hole_loops.resize(face_holes[i].size());
      for (unsigned j = 0; j < face_holes[i].size(); ++j) {
        face_hole_loops[j].swap(hole_loops[face_holes[i][j]]);
      }
      if (face_hole_loops.size()) {

        f_loops.push_back(carve::triangulate::incorporateHolesIntoPolygon(face->projector(), face_loops[i], face_hole_loops));
      } else {
        f_loops.push_back(face_loops[i]);
      }
    }

#else
    // use new 2d-only hole patching code.
    for (size_t i = 0; i < face_loops.size(); ++i) {
      if (!face_holes[i].size()) {
        f_loops.push_back(face_loops[i]);
        continue;
      }

      std::vector<std::vector<carve::geom2d::P2> > projected_poly;
      projected_poly.resize(face_holes[i].size() + 1);
      projected_poly[0].reserve(face_loops[i].size());
      for (size_t j = 0; j < face_loops[i].size(); ++j) {
        projected_poly[0].push_back(face->project(face_loops[i][j]->v));
      }
      for (size_t j = 0; j < face_holes[i].size(); ++j) {
        projected_poly[j+1].reserve(hole_loops[face_holes[i][j]].size());
        for (size_t k = 0; k < hole_loops[face_holes[i][j]].size(); ++k) {
          projected_poly[j+1].push_back(face->project(hole_loops[face_holes[i][j]][k]->v));
        }
      }

      std::vector<std::pair<size_t, size_t> > result = carve::triangulate::incorporateHolesIntoPolygon(projected_poly);
      f_loops.push_back(std::vector<const poly_t::vertex_t *>());
      std::vector<const poly_t::vertex_t *> &out = f_loops.back();
      out.reserve(result.size());
      for (size_t j = 0; j < result.size(); ++j) {
        if (result[j].first == 0) {
          out.push_back(face_loops[i][result[j].second]);
        } else {
          out.push_back(hole_loops[face_holes[i][result[j].first-1]][result[j].second]);
        }
      }
    }
#endif
  }



  /** 
   * \brief Assemble the base loop for a face.
   *
   * The base loop is the original face loop, including vertices
   * created by intersections crossing any of its edges.
   * 
   * @param[in] face The face to process.
   * @param[in] vmap 
   * @param[in] face_split_edges 
   * @param[in] divided_edges A mapping from edge pointer to sets of
   *            ordered vertices corrsponding to the intersection points
   *            on that edge.
   * @param[out] base_loop A vector of the vertices of the base loop.
   */
  static void assembleBaseLoop(const poly_t::face_t *face,
                               const carve::csg::detail::Data &data,
                               std::vector<const poly_t::vertex_t *> &base_loop) {
    base_loop.clear();

    // XXX: assumes that face->edges is in the same order as
    // face->vertices. (Which it is)
    for (size_t j = 0, je = face->vertices.size(); j < je; ++j) {
      base_loop.push_back(carve::csg::map_vertex(data.vmap, face->vertices[j]));

      const poly_t::edge_t *e = face->edges[j];
      carve::csg::detail::EVVMap::const_iterator ev = data.divided_edges.find(e);

      if (ev != data.divided_edges.end()) {
        const std::vector<const poly_t::vertex_t *> &ev_vec = ((*ev).second);

        if (e->v1 == face->vertices[j]) {
          // edge is forward;
          for (size_t k = 0, ke = ev_vec.size(); k < ke;) {
            base_loop.push_back(ev_vec[k++]);
          }
        } else {
          // edge is backward;
          for (size_t k = ev_vec.size(); k;) {
            base_loop.push_back(ev_vec[--k]);
          }
        }
      }
    }
  }



  struct crossing_data {
    std::vector<const poly_t::vertex_t *> *path;
    size_t edge_idx[2];

    crossing_data(std::vector<const poly_t::vertex_t *> *p, size_t e1, size_t e2) : path(p) {
      edge_idx[0] = e1; edge_idx[1] = e2;
    }
    bool operator<(const crossing_data &c) const {
      return edge_idx[0] < c.edge_idx[0] || (edge_idx[0] == c.edge_idx[0] && edge_idx[1] > c.edge_idx[1]);
    }
  };



  static inline bool internalToAngle(const carve::geom2d::P2 &a,
                                     const carve::geom2d::P2 &b,
                                     const carve::geom2d::P2 &c,
                                     const carve::geom2d::P2 &p) {
    bool reflex = (a < c) ?
      carve::geom2d::orient2d(a, b, c) <= 0.0 :
      carve::geom2d::orient2d(c, b, a) >= 0.0;
    if (reflex) {
      return
        carve::geom2d::orient2d(a, b, p) >= 0.0 ||
        carve::geom2d::orient2d(b, c, p) >= 0.0;
    } else {
      return
        carve::geom2d::orient2d(a, b, p) > 0.0 &&
        carve::geom2d::orient2d(b, c, p) > 0.0;
    }
  }



  bool processCrossingEdges(const poly_t::face_t *face,
                            const carve::csg::VertexIntersections &vertex_intersections,
                            carve::csg::CSG::Hooks &hooks,
                            std::vector<const poly_t::vertex_t *> &base_loop,
                            std::vector<std::vector<const poly_t::vertex_t *> > &paths,
                            std::vector<std::vector<const poly_t::vertex_t *> > &loops,
                            std::list<std::vector<const poly_t::vertex_t *> > &face_loops_out) {
    const size_t N = base_loop.size();
    std::vector<crossing_data> endpoint_indices;

    endpoint_indices.reserve(paths.size());

    for (size_t i = 0; i < paths.size(); ++i) {
      endpoint_indices.push_back(crossing_data(&paths[i], N, N));
    }

    // locate endpoints of paths on the base loop.
    for (size_t i = 0; i < N; ++i) {
      for (size_t j = 0; j < paths.size(); ++j) {
        if (paths[j].front() == base_loop[i]) {
          if (endpoint_indices[j].edge_idx[0] == N) {
            endpoint_indices[j].edge_idx[0] = i;
          } else {
            // have to work out which of the duplicated vertices is the right one to attach to.
            const std::vector<const poly_t::vertex_t *> &p = *endpoint_indices[j].path;
            const size_t pN = p.size();

            const poly_t::vertex_t *a, *b, *c;
            a = base_loop[(i+N-1)%N];
            b = base_loop[i];
            c = base_loop[(i+1)%N];

            const poly_t::vertex_t *adj = (p[0] == base_loop[i]) ? p[1] : p[pN-2];

            if (internalToAngle(face->project(a->v),
                                face->project(b->v),
                                face->project(c->v),
                                face->project(adj->v))) {
              endpoint_indices[j].edge_idx[0] = i;
            }
          }
        }
        if (paths[j].back()  == base_loop[i]) {
          if (endpoint_indices[j].edge_idx[1] == N) {
            endpoint_indices[j].edge_idx[1] = i;
          } else {
            // have to work out which of the duplicated vertices is the right one to attach to.
            const std::vector<const poly_t::vertex_t *> &p = *endpoint_indices[j].path;
            const size_t pN = p.size();

            const poly_t::vertex_t *a, *b, *c;
            a = base_loop[(i+N-1)%N];
            b = base_loop[i];
            c = base_loop[(i+1)%N];

            const poly_t::vertex_t *adj = (p[0] == base_loop[i]) ? p[1] : p[pN-2];

            if (internalToAngle(face->project(a->v),
                                face->project(b->v),
                                face->project(c->v),
                                face->project(adj->v))) {
              endpoint_indices[j].edge_idx[1] = i;
            }
          }
        }
      }
    }

    // divide paths up into those that connect to the base loop in two
    // places, and those that do not.
    std::vector<crossing_data> cross, noncross;
    cross.reserve(endpoint_indices.size() + 1);
    noncross.reserve(endpoint_indices.size());

    for (size_t i = 0; i < endpoint_indices.size(); ++i) {
      if (endpoint_indices[i].edge_idx[0] == endpoint_indices[i].edge_idx[1]) {
        // in this case, we need to orient the path so that the constructed loop has the right orientation.
        double area = carve::geom2d::signedArea(endpoint_indices[i].path->begin() + 1,
                                                endpoint_indices[i].path->end(),
                                                face->projector());
        if (area < 0) {
          std::reverse(endpoint_indices[i].path->begin(), endpoint_indices[i].path->end());
        }
      } else if (endpoint_indices[i].edge_idx[0] > endpoint_indices[i].edge_idx[1]) {
        std::swap(endpoint_indices[i].edge_idx[0], endpoint_indices[i].edge_idx[1]);
        std::reverse(endpoint_indices[i].path->begin(), endpoint_indices[i].path->end());
      }

      if (endpoint_indices[i].edge_idx[1] != N) {
        cross.push_back(endpoint_indices[i]);
      } else {
        noncross.push_back(endpoint_indices[i]);
      }
    }

    // add a temporary crossing path that connects the beginning and the
    // end of the base loop. this stops us from needing special case
    // code to handle the left over loop after all the other crossing
    // paths are considered.
    std::vector<const poly_t::vertex_t *> base_loop_temp_path;
    base_loop_temp_path.reserve(2);
    base_loop_temp_path.push_back(base_loop.front());
    base_loop_temp_path.push_back(base_loop.back());

    cross.push_back(crossing_data(&base_loop_temp_path, 0, base_loop.size() - 1));

    // sort paths by increasing beginning point and decreasing ending point.
    std::sort(cross.begin(), cross.end());
    std::sort(noncross.begin(), noncross.end());

    // divide up the base loop based upon crossing paths.
    std::vector<std::vector<const poly_t::vertex_t *> > divided_base_loop;
    divided_base_loop.reserve(cross.size());
    std::vector<const poly_t::vertex_t *> out;

    for (size_t i = 0; i < cross.size(); ++i) {
      size_t j;
      for (j = i + 1;
           j < cross.size() &&
             cross[i].edge_idx[0] == cross[j].edge_idx[0] && 
             cross[i].edge_idx[1] == cross[j].edge_idx[1];
           ++j) {}
      if (j - i >= 2) {
        // when there are multiple paths that begin and end at the
        // same point, they need to be ordered so that the constructed
        // loops have the right orientation. this means that the loop
        // made by taking path(i+1) forward, then path(i) backward
        // needs to have negative area. this combined area is equal to
        // the area of path(i+1) minus the area of path(i). in turn
        // this means that the loop made by path path(i+1) alone has
        // to have smaller signed area than loop made by path(i).
        // thus, we sort paths in order of decreasing area.
        std::vector<std::pair<double, std::vector<const poly_t::vertex_t *> *> > order;
        order.reserve(j - i);
        for (size_t k = i; k < j; ++k) {
          double area = carve::geom2d::signedArea(cross[k].path->begin(),
                                                  cross[k].path->end(),
                                                  face->projector());
          order.push_back(std::make_pair(-area, cross[k].path));
        }
        std::sort(order.begin(), order.end());
        for (size_t k = i; k < j; ++k) {
          cross[k].path = order[k-i].second;
        }
      }
    }

    for (size_t i = 0; i < cross.size(); ++i) {
      size_t e1_0 = cross[i].edge_idx[0];
      size_t e1_1 = cross[i].edge_idx[1];
      std::vector<const poly_t::vertex_t *> &p1 = *cross[i].path;

      out.clear();

      if (i < cross.size() - 1 &&
          cross[i+1].edge_idx[0] < cross[i].edge_idx[1]) {
        // complex case. crossing path with other crossing paths embedded within.
        size_t pos = e1_0;

        size_t skip = i+1;

        while (pos != e1_1) {

          std::vector<const poly_t::vertex_t *> &p2 = *cross[skip].path;
          size_t e2_0 = cross[skip].edge_idx[0];
          size_t e2_1 = cross[skip].edge_idx[1];

          // copy up to the beginning of the next path.
          std::copy(base_loop.begin() + pos, base_loop.begin() + e2_0, std::back_inserter(out));

          CARVE_ASSERT(base_loop[e2_0] == p2[0]);
          // copy the next path in the right direction.
          std::copy(p2.begin(), p2.end() - 1, std::back_inserter(out));

          // move to the position of the end of the path.
          pos = e2_1;

          // advance to the next hit path.
          do {
            ++skip;
          } while(skip != cross.size() && cross[skip].edge_idx[0] < e2_1);

          if (skip == cross.size()) break;

          // if the next hit path is past the start point of the current path, we're done.
          if (cross[skip].edge_idx[0] >= e1_1) break;
        }

        // copy up to the end of the path.
        std::copy(base_loop.begin() + pos, base_loop.begin() + e1_1, std::back_inserter(out));

        CARVE_ASSERT(base_loop[e1_1] == p1.back());
        std::copy(p1.rbegin(), p1.rend() - 1, std::back_inserter(out));
      } else {
        size_t loop_size = (e1_1 - e1_0) + (p1.size() - 1);
        out.reserve(loop_size);

        std::copy(base_loop.begin() + e1_0, base_loop.begin() + e1_1, std::back_inserter(out));
        std::copy(p1.rbegin(), p1.rend() - 1, std::back_inserter(out));

        CARVE_ASSERT(out.size() == loop_size);
      }
      divided_base_loop.push_back(out);
    }

    // for each divided base loop, work out which noncrossing paths and
    // loops are part of it. use the old algorithm to combine these into
    // the divided base loop. if none, the divided base loop is just
    // output.
    std::vector<std::vector<carve::geom2d::P2> > proj;
    std::vector<carve::geom::aabb<2> > proj_aabb;
    proj.resize(divided_base_loop.size());
    proj_aabb.resize(divided_base_loop.size());

    // calculate an aabb for each divided base loop, to avoid expensive
    // point-in-poly tests.
    for (size_t i = 0; i < divided_base_loop.size(); ++i) {
      proj[i].reserve(divided_base_loop[i].size());
      for (size_t j = 0; j < divided_base_loop[i].size(); ++j) {
        proj[i].push_back(face->project(divided_base_loop[i][j]->v));
      }
      proj_aabb[i].fit(proj[i].begin(), proj[i].end());
    }

    for (size_t i = 0; i < divided_base_loop.size(); ++i) {
      std::vector<std::vector<const poly_t::vertex_t *> *> inc;
      carve::geom2d::P2 test;

      // for each noncrossing path, choose an endpoint that isn't on the
      // base loop as a test point.
      for (size_t j = 0; j < noncross.size(); ++j) {
        if (noncross[j].edge_idx[0] < N) {
          if (noncross[j].path->front() == base_loop[noncross[j].edge_idx[0]]) {
            test = face->project(noncross[j].path->back()->v);
          } else {
            test = face->project(noncross[j].path->front()->v);
          }
        } else {
          test = face->project(noncross[j].path->front()->v);
        }

        if (proj_aabb[i].intersects(test) &&
            carve::geom2d::pointInPoly(proj[i], test).iclass != carve::POINT_OUT) {
          inc.push_back(noncross[j].path);
        }
      }

      // for each loop, just test with any point.
      for (size_t j = 0; j < loops.size(); ++j) {
        test = face->project(loops[j].front()->v);

        if (proj_aabb[i].intersects(test) &&
            carve::geom2d::pointInPoly(proj[i], test).iclass != carve::POINT_OUT) {
          inc.push_back(&loops[j]);
        }
      }

      if (inc.size()) {
        carve::csg::V2Set face_edges;

        for (size_t j = 0; j < divided_base_loop[i].size() - 1; ++j) {
          face_edges.insert(std::make_pair(divided_base_loop[i][j],
                                           divided_base_loop[i][j+1]));
        }

        face_edges.insert(std::make_pair(divided_base_loop[i].back(),
                                         divided_base_loop[i].front()));

        for (size_t j = 0; j < inc.size(); ++j) {
          std::vector<const poly_t::vertex_t *> &path = *inc[j];
          for (size_t k = 0; k < path.size() - 1; ++k) {
            face_edges.insert(std::make_pair(path[k], path[k+1]));
            face_edges.insert(std::make_pair(path[k+1], path[k]));
          }
        }

        std::list<std::vector<const poly_t::vertex_t *> > face_loops;
        std::list<std::vector<const poly_t::vertex_t *> > hole_loops;

        splitFace(face, face_edges, face_loops, hole_loops, vertex_intersections);

        if (hole_loops.size()) {
          mergeFacesAndHoles(face, face_loops, hole_loops, hooks);
        }
        std::copy(face_loops.begin(), face_loops.end(), std::back_inserter(face_loops_out));
      } else {
        face_loops_out.push_back(divided_base_loop[i]);
      }
    }
  }



  template<typename T>
  void populateVectorFromList(std::list<T> &l, std::vector<T> &v) {
    v.clear();
    v.reserve(l.size());
    for (typename std::list<T>::iterator i = l.begin(); i != l.end(); ++i) {
      v.push_back(T());
      std::swap(*i, v.back());
    }
    l.clear();
  }



  void composeEdgesIntoPaths(const carve::csg::V2Set &edges,
                             const std::vector<const poly_t::vertex_t *> &extra_endpoints,
                             std::vector<std::vector<const poly_t::vertex_t *> > &paths,
                             std::vector<std::vector<const poly_t::vertex_t *> > &loops) {
    using namespace carve::csg;

    detail::VVSMap vertex_graph;
    detail::VSet endpoints;

    std::vector<const poly_t::vertex_t *> path;

    std::list<std::vector<const poly_t::vertex_t *> > temp;

    // build graph from edges.
    for (V2Set::const_iterator i = edges.begin(); i != edges.end(); ++i) {
      vertex_graph[(*i).first].insert((*i).second);
      vertex_graph[(*i).second].insert((*i).first);
    }

    // find the endpoints in the graph.
    for (detail::VVSMap::const_iterator i = vertex_graph.begin(); i != vertex_graph.end(); ++i) {
      if ((*i).second.size() != 2) {
        endpoints.insert((*i).first);
      }
    }

    for (size_t i = 0; i < extra_endpoints.size(); ++i) {
      if (vertex_graph.find(extra_endpoints[i]) != vertex_graph.end()) {
        endpoints.insert(extra_endpoints[i]);
      }
    }

    while (endpoints.size()) {
      const poly_t::vertex_t *v = *endpoints.begin();
      detail::VVSMap::iterator p = vertex_graph.find(v);
      if (p == vertex_graph.end()) {
        endpoints.erase(endpoints.begin());
        continue;
      }

      path.clear();
      path.push_back(v);

      while (1) {
        CARVE_ASSERT(p != vertex_graph.end());

        // pick a connected vertex to move to.
        if ((*p).second.size() == 0) break;

        const poly_t::vertex_t *n = *((*p).second.begin());
        detail::VVSMap::iterator q = vertex_graph.find(n);

        // remove the link.
        (*p).second.erase(n);
        (*q).second.erase(v);

        // move on.
        v = n;
        path.push_back(v);

        if ((*p).second.size() == 0) vertex_graph.erase(p);
        if ((*q).second.size() == 0) {
          vertex_graph.erase(q);
          q = vertex_graph.end();
        }

        p = q;

        if (v == path[0] || p == vertex_graph.end() || endpoints.find(v) != endpoints.end()) break;
      }
      CARVE_ASSERT(endpoints.find(path.back()) != endpoints.end());

      temp.push_back(path);
    }
    populateVectorFromList(temp, paths);

    temp.clear();
    // now only loops should remain in the graph.
    while (vertex_graph.size()) {
      detail::VVSMap::iterator p = vertex_graph.begin();
      const poly_t::vertex_t *v = (*p).first;
      CARVE_ASSERT((*p).second.size() == 2);

      std::vector<const poly_t::vertex_t *> path;
      path.clear();
      path.push_back(v);

      while (1) {
        CARVE_ASSERT(p != vertex_graph.end());
        // pick a connected vertex to move to.

        const poly_t::vertex_t *n = *((*p).second.begin());
        detail::VVSMap::iterator q = vertex_graph.find(n);

        // remove the link.
        (*p).second.erase(n);
        (*q).second.erase(v);

        // move on.
        v = n;
        path.push_back(v);

        if ((*p).second.size() == 0) vertex_graph.erase(p);
        if ((*q).second.size() == 0) vertex_graph.erase(q);

        p = q;

        if (v == path[0]) break;
      }

      temp.push_back(path);
    }
    populateVectorFromList(temp, loops);
  }



#if defined(CARVE_DEBUG_WRITE_PLY_DATA)
  void dumpFacesAndHoles(const std::list<std::vector<const poly_t::vertex_t *> > &face_loops,
                         const std::list<std::vector<const poly_t::vertex_t *> > &hole_loops) {
    std::map<const poly_t::vertex_t *, size_t> v_included;

    for (std::list<std::vector<const poly_t::vertex_t *> >::const_iterator
           i = face_loops.begin(); i != face_loops.end(); ++i) {
      for (size_t j = 0; j < (*i).size(); ++j) {
        if (v_included.find((*i)[j]) == v_included.end()) {
          size_t &p = v_included[(*i)[j]];
          p = v_included.size() - 1;
        }
      }
    }

    for (std::list<std::vector<const poly_t::vertex_t *> >::const_iterator
           i = hole_loops.begin(); i != hole_loops.end(); ++i) {
      for (size_t j = 0; j < (*i).size(); ++j) {
        if (v_included.find((*i)[j]) == v_included.end()) {
          size_t &p = v_included[(*i)[j]];
          p = v_included.size() - 1;
        }
      }
    }

    carve::line::PolylineSet fh;
    fh.vertices.resize(v_included.size());
    for (std::map<const poly_t::vertex_t *, size_t>::const_iterator
           i = v_included.begin(); i != v_included.end(); ++i) {
      fh.vertices[(*i).second].v = (*i).first->v;
    }

    {
      std::vector<size_t> connected;
      for (std::list<std::vector<const poly_t::vertex_t *> >::const_iterator
             i = face_loops.begin(); i != face_loops.end(); ++i) {
        connected.clear();
        for (size_t j = 0; j < (*i).size(); ++j) {
          connected.push_back(v_included[(*i)[j]]);
        }
        fh.addPolyline(true, connected.begin(), connected.end());
      }
      for (std::list<std::vector<const poly_t::vertex_t *> >::const_iterator
             i = hole_loops.begin(); i != hole_loops.end(); ++i) {
        connected.clear();
        for (size_t j = 0; j < (*i).size(); ++j) {
          connected.push_back(v_included[(*i)[j]]);
        }
        fh.addPolyline(true, connected.begin(), connected.end());
      }
    }

    std::string out("/tmp/hole_merge.ply");
    ::writePLY(out, &fh, true);
  }
#endif



  void generateOneFaceLoop(const poly_t::face_t *face,
                           const carve::csg::detail::Data &data,
                           const carve::csg::VertexIntersections &vertex_intersections,
                           carve::csg::CSG::Hooks &hooks,
                           std::list<std::vector<const poly_t::vertex_t *> > &face_loops) {
    using namespace carve::csg;

    std::vector<const poly_t::vertex_t *> base_loop;
    std::list<std::vector<const poly_t::vertex_t *> > hole_loops;

    assembleBaseLoop(face, data, base_loop);

    detail::FV2SMap::const_iterator fse_iter = data.face_split_edges.find(face);

    face_loops.clear();

    if (fse_iter == data.face_split_edges.end()) {
      // simple case: input face is output face (possibly with the
      // addition of vertices at intersections).
      face_loops.push_back(base_loop);
      return;
    }

    // complex case: input face is split into multiple output faces.
    V2Set face_edges;

    for (size_t j = 0, je = base_loop.size() - 1; j < je; ++j) {
      face_edges.insert(std::make_pair(base_loop[j], base_loop[j + 1]));
    }
    face_edges.insert(std::make_pair(base_loop.back(), base_loop[0]));

    // collect the split edges (as long as they're not on the perimeter)
    const detail::FV2SMap::mapped_type &fse = ((*fse_iter).second);

    V2Set split_edges;

    for (detail::FV2SMap::mapped_type::const_iterator
           j = fse.begin(), je =  fse.end();
         j != je;
         ++j) {
      const poly_t::vertex_t *v1 = ((*j).first), *v2 = ((*j).second);

      if (face_edges.find(std::make_pair(v1, v2)) == face_edges.end() &&
          face_edges.find(std::make_pair(v2, v1)) == face_edges.end()) {

        split_edges.insert(ordered_edge(v1, v2));
      }
    }

    // face is unsplit.
    if (!split_edges.size()) {
      face_loops.push_back(base_loop);
      return;
    }

    if (split_edges.size() == 1) {
      const poly_t::vertex_t *v1 = split_edges.begin()->first;
      const poly_t::vertex_t *v2 = split_edges.begin()->second;

      std::vector<const poly_t::vertex_t *>::iterator vi1 = std::find(base_loop.begin(), base_loop.end(), v1);
      std::vector<const poly_t::vertex_t *>::iterator vi2 = std::find(base_loop.begin(), base_loop.end(), v2);

      if (vi1 != base_loop.end() && vi2 != base_loop.end()) {
        // this is an inserted edge that connects two points on the base loop. nice and simple.
        if (vi2 < vi1) std::swap(vi1, vi2);

        size_t loop1_size = vi2 - vi1 + 1;
        size_t loop2_size = base_loop.size() + 2 - loop1_size;

        std::vector<const poly_t::vertex_t *> l1;
        std::vector<const poly_t::vertex_t *> l2;

        l1.reserve(loop1_size);
        l2.reserve(loop2_size);

        std::copy(vi1, vi2+1, std::back_inserter(l1));
        std::copy(vi2, base_loop.end(), std::back_inserter(l2));
        std::copy(base_loop.begin(), vi1+1, std::back_inserter(l2));

        CARVE_ASSERT(l1.size() == loop1_size);
        CARVE_ASSERT(l2.size() == loop2_size);

        face_loops.push_back(l1);
        face_loops.push_back(l2);

        return;
      }
    }

    std::vector<std::vector<const poly_t::vertex_t *> > paths;
    std::vector<std::vector<const poly_t::vertex_t *> > loops;

    composeEdgesIntoPaths(split_edges, base_loop, paths, loops);

    if (!paths.size()) {
      // loops found by composeEdgesIntoPaths() can't touch the boundary, or each other, so we can deal with the no paths case simply.
      // the hole loops are the loops produced by composeEdgesIntoPaths() oriented so that their signed area wrt. the face is negative.
      // the face loops are the base loop plus the hole loops, reversed.
      face_loops.push_back(base_loop);

      for (size_t i = 0; i < loops.size(); ++i) {
        hole_loops.push_back(std::vector<const poly_t::vertex_t *>());
        hole_loops.back().reserve(loops[i].size()-1);
        std::copy(loops[i].begin(), loops[i].end()-1, std::back_inserter(hole_loops.back()));

        face_loops.push_back(std::vector<const poly_t::vertex_t *>());
        face_loops.back().reserve(loops[i].size()-1);
        std::copy(loops[i].rbegin()+1, loops[i].rend(), std::back_inserter(face_loops.back()));

        std::vector<carve::geom2d::P2> projected;
        projected.reserve(face_loops.back().size());
        for (size_t i = 0; i < face_loops.back().size(); ++i) {
          projected.push_back(face->project(face_loops.back()[i]->v));
        }

        if (carve::geom2d::signedArea(projected) > 0.0) {
          std::swap(face_loops.back(), hole_loops.back());
        }
      }

      // if there are holes, then they need to be merged with faces.
      if (hole_loops.size()) {
        mergeFacesAndHoles(face, face_loops, hole_loops, hooks);
      }
    } else {
      if (!processCrossingEdges(face, vertex_intersections, hooks, base_loop, paths, loops, face_loops)) {
        // complex case - fall back to old edge tracing code.
        for (V2Set::const_iterator i = split_edges.begin(); i != split_edges.end(); ++i) {
          face_edges.insert(std::make_pair((*i).first, (*i).second));
          face_edges.insert(std::make_pair((*i).second, (*i).first));
        }
        splitFace(face, face_edges, face_loops, hole_loops, vertex_intersections);

        if (hole_loops.size()) {
          mergeFacesAndHoles(face, face_loops, hole_loops, hooks);
        }
      }
    }
  }



}



/** 
 * \brief Build a set of face loops for all (split) faces of a Polyhedron.
 * 
 * @param[in] poly The polyhedron to process
 * @param vmap 
 * @param face_split_edges 
 * @param divided_edges 
 * @param[out] face_loops_out The resulting face loops
 * 
 * @return The number of edges generated.
 */
size_t carve::csg::CSG::generateFaceLoops(const poly_t *poly,
                                          const detail::Data &data,
                                          FaceLoopList &face_loops_out) {
  static carve::TimingName FUNC_NAME("CSG::generateFaceLoops()");
  carve::TimingBlock block(FUNC_NAME);
  size_t generated_edges = 0;
  std::vector<const poly_t::vertex_t *> base_loop;
  std::list<std::vector<const poly_t::vertex_t *> > face_loops, hole_loops;
  
  for (std::vector<poly_t::face_t >::const_iterator
         i = poly->faces.begin(), e = poly->faces.end();
       i != e;
       ++i) {
    const poly_t::face_t *face = &(*i);

    generateOneFaceLoop(face, data, vertex_intersections, hooks, face_loops);

    // now record all the resulting face loops.
    for (std::list<std::vector<const poly_t::vertex_t *> >::const_iterator
           f = face_loops.begin(), fe = face_loops.end();
         f != fe;
         ++f) {
      face_loops_out.append(new FaceLoop(face, *f));
      generated_edges += (*f).size();
    }
  }
  return generated_edges;
}
