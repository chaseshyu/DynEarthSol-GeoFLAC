#include "algorithm"
#include "iostream"

#include "ANN/ANN.h"

#include "constants.hpp"
#include "parameters.hpp"

#include "barycentric-fn.hpp"
#include "utils.hpp"
#include "brc-interpolation.hpp"

namespace { // anonymous namespace

typedef Array2D<double,NODES_PER_ELEM> brc_t;


void interpolate_field(const brc_t &brc, const int_vec &el, const conn_t &connectivity,
                       const double_vec &source, double_vec &target)
{
    #pragma omp parallel for default(none)          \
        shared(brc, el, connectivity, source, target)
    for (int i=0; i<target.size(); i++) {
        int e = el[i];
        const int *conn = connectivity[e];
        double result = 0;
        for (int j=0; j<NODES_PER_ELEM; j++) {
            result += source[conn[j]] * brc[i][j];
        }
        target[i] = result;
    }
}


void interpolate_field(const brc_t &brc, const int_vec &el, const conn_t &connectivity,
                       const array_t &source, array_t &target)
{
    #pragma omp parallel for default(none)          \
        shared(brc, el, connectivity, source, target)
    for (int i=0; i<target.size(); i++) {
        int e = el[i];
        const int *conn = connectivity[e];
        for (int d=0; d<NDIMS; d++) {
            double result = 0;
            for (int j=0; j<NODES_PER_ELEM; j++) {
                result += source[conn[j]][d] * brc[i][j];
            }
            target[i][d] = result;
        }
    }
}


void prepare_interpolation(const Variables &var, const array_t &old_coord,
                           const conn_t &old_connectivity,
                           brc_t &brc, int_vec &el)
{
    // for each new coord point, find the enclosing old element

    Barycentric_transformation bary(old_coord, old_connectivity, *var.volume);

    // ANN requires double** as input
    double **points = new double*[old_coord.size()];
    for (int i=0; i<old_coord.size(); i++) {
        points[i] = const_cast<double*>(old_coord[i]);
    }
    ANNkd_tree kdtree(points, old_coord.size(), NDIMS);

    const std::vector<int_vec> &old_support = *var.support;

    const int k = 1;
    const double eps = 0;
    int *nn_idx = new int[k];
    double *dd = new double[k];

    // Note: kdtree.annkSearch() is not thread-safe, cannot use openmp in this loop
    for (int i=0; i<var.nnode; i++) {
        double *q = (*var.coord)[i];
        // find the nearest point nn in old_coord
        kdtree.annkSearch(q, k, nn_idx, dd, eps);
        int nn = nn_idx[0];

        // std::cout << i << " ";
        // print(std::cout, q, NDIMS);
        // std::cout << " " << nn << " " << dd[0] << '\n';

        double r[NDIMS];
        const int_vec &nn_elem = old_support[nn];
        int e;

        // shortcut: q is exactly the same as nn
        if (dd[0] == 0) {
            e = nn_elem[0];
            bary.transform(q, e, r);
            // r should be a permutation of [1, 0, 0]
            // normalize r to remove round-off error
            for (int d=0; d<NDIMS; d++) {
                if (r[d] > 0.9)
                    r[d] = 1;
                else
                    r[d] = 0;
            }
            goto found;
        }

        // loop over (old) elements surrounding nn to find
        // the element that is enclosing q
        for (int j=0; j<nn_elem.size(); j++) {
            e = nn_elem[j];
            bary.transform(q, e, r);
            if (bary.is_inside(r)) {
                // std::cout << e << " ";
                // print(std::cout, r, NDIMS);
                // std::cout << '\n';
                goto found;
            }
        }
    not_found:
        {
            // Situation: q is in the upper element, but its nearest point is o!
            // we won't find the enclosing element with the method above
            //     x
            //    / \   <-- this is a large triangle
            //   / q \
            //  x---- x
            //   \-o-/   <-- this is a small triangle
            //

            // this array contains the elements that have been searched so far
            int_vec searched;
            for (int j=0; j<nn_elem.size(); j++) {
                searched.push_back(nn_elem[j]);
            }
            // print(std::cout, searched);
            // std::cout << " ... \n";

            // search through elements that are neighbors of nn_elem
            for (int j=0; j<nn_elem.size(); j++) {
                int ee = nn_elem[j];
                const int *conn = old_connectivity[ee];
                for (int m=0; m<NODES_PER_ELEM; m++) {
                    // np is a node close to q
                    int np = conn[m];
                    const int_vec &np_elem = old_support[np];
                    for (int j=0; j<np_elem.size(); j++) {
                        e = np_elem[j];
                        auto it = std::find(searched.begin(), searched.end(), e);
                        if (it != searched.end()) {
                            // this element has been searched before
                            continue;
                        }
                        searched.push_back(e);
                        bary.transform(q, e, r);
                        // std::cout << e << " ";
                        // print(std::cout, r, NDIMS);
                        // std::cout << " ... \n";
                        if (bary.is_inside(r)) {
                            goto found;
                        }
                    }
                }
            }
        }
        {
            std::cout << "New node is outside of the old domain. \n";

            // Situation: q must be outside the old domain
            // using nearest old_coord instead
            bary.transform(points[nn], nn_elem[0], r);
        }
    found:
        el[i] = e;
        double sum = 0;
        for (int d=0; d<NDIMS; d++) {
            brc[i][d] = r[d];
            sum += r[d];
        }
        brc[i][NODES_PER_ELEM-1] = 1 - sum;
    }

    delete [] nn_idx;
    delete [] dd;
    delete [] points;

    // print(std::cout, *var.coord);
    // std::cout << '\n';
    // print(std::cout, el);
    // std::cout << '\n';
    // print(std::cout, bar);
    // std::cout << '\n';
}

} // end of anonymous namespace


void barycentric_node_interpolation(Variables &var, const array_t &old_coord,
                                    const conn_t &old_connectivity)
{
    int_vec el(var.nnode);
    brc_t brc(var.nnode);
    prepare_interpolation(var, old_coord, old_connectivity, brc, el);

    double_vec *a = new double_vec(var.nnode);
    interpolate_field(brc, el, old_connectivity, *var.temperature, *a);
    delete var.temperature;
    var.temperature = a;

    array_t *b = new array_t(var.nnode);
    interpolate_field(brc, el, old_connectivity, *var.vel, *b);
    delete var.vel;
    var.vel = b;
}


