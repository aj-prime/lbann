"""Utilities to interact with SNAP.

SNAP is the Stanford Network Analysis Platform. See
https://snap.stanford.edu.

"""
import gzip
import os
import os.path
import subprocess
import urllib.request
import numpy as np

# Root directory for LBANN graph application
_root_dir = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))


def download_graph(name='youtube',
                   graph_file=None,
                   reorder_indices=True):
    """Download graph edgelist file from SNAP website.

    Returns immediately if graph has already been downloaded.

    Args:
        name (str): Name of graph.
        graph_file (str, optional): File where uncompressed edge list
            will be saved (default: in 'data/graphs' directory).

    Returns:
        str: Uncompressed edge list file.

    """

    # Return immediately if graph is already downloaded
    if not graph_file:
        graph_file = os.path.join(_root_dir, 'data', 'graphs', name, 'graph.txt')
    if os.path.exists(graph_file):
        return graph_file
    graph_file = os.path.realpath(graph_file)

    # Graphs URLs
    download_urls = {
        'facebook': 'http://snap.stanford.edu/data/facebook_combined.txt.gz',
        'youtube': 'https://snap.stanford.edu/data/bigdata/communities/com-youtube.ungraph.txt.gz',
        'friendster': 'https://snap.stanford.edu/data/bigdata/communities/com-friendster.ungraph.txt.gz',
    }

    # Paths
    data_dir = os.path.dirname(graph_file)
    if not os.path.isdir(data_dir):
        os.makedirs(data_dir)
    data_dir = os.path.realpath(data_dir)
    compressed_file = graph_file + '.gz'

    # Download and uncompress graph file
    urllib.request.urlretrieve(download_urls[name],
                               filename=compressed_file)
    if reorder_indices:
        edges = np.loadtxt(compressed_file, dtype=np.uint64)
        index_map = {}
        for v in np.nditer(edges):
            v = v[()]
            if v not in index_map:
                index_map[v] = np.uint64(len(index_map))
        edges = np.vectorize(index_map.get)(edges)
        np.savetxt(graph_file, edges, fmt='%u', delimiter='\t')
    else:
        with gzip.open(compressed_file, 'rb') as in_file:
            with open(graph_file, 'wb') as out_file:
                out_file.write(in_file.read())

    return graph_file


def node2vec_walk(graph_file,
                  walk_file,
                  walk_length,
                  walks_per_node,
                  return_param=1.0,
                  inout_param=1.0,
                  directed=False,
                  weighted=False,
                  verbose=False):
    """Perform random walk on graph for node2vec.

    See https://cs.stanford.edu/~jure/pubs/node2vec-kdd16.pdf

    Args:
        graph_file (str): Uncompressed edge list file.
        walk_file (str): File where random walks will be saved.
        walk_length (int): Walk length.
        walks_per_node (int): Number of walks per graph vertex.
        return_param (float, optional): p-parameter for random walk
            (default: 1.0).
        inout_param (float, optional): q-parameter for random walk
            (default: 1.0).
        directed (bool, optional): Graph is directed (default: False).
        weighted (bool, optional): Graph is weighted (default: False).
        verbose (bool, optional): Verbose output (default: False).

    """

    # Check executable
    node2vec_exe = os.path.join(_root_dir, 'snap', 'examples',
                                'node2vec', 'node2vec')
    if not os.path.isfile(node2vec_exe):
        raise FileNotFoundError(
            'Could not find node2vec executable at {}. '
            'Has SNAP been built?'
            .format(node2vec_exe)
        )

    # Make sure output directory exists
    output_dir = os.path.dirname(os.path.realpath(walk_file))
    if not os.path.isdir(output_dir):
        os.makedirs(output_dir)

    # Construct invocation
    command = [
        node2vec_exe,
        '-i:{}'.format(graph_file),
        '-o:{}'.format(walk_file),
        '-d:-1',
        '-l:{}'.format(walk_length),
        '-r:{}'.format(walks_per_node),
        '-k:-1',
        '-e:-1',
        '-p:{}'.format(return_param),
        '-q:{}'.format(inout_param),
        '-ow',
    ]
    if verbose:
        command.append('-v')
    if directed:
        command.append('-dr')
    if weighted:
        command.append('-w')

    # Run executable
    return subprocess.call(command)
