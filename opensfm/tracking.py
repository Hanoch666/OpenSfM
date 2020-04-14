import logging
import sys

import numpy as np
import networkx as nx

from collections import defaultdict
from itertools import combinations
from six import iteritems

from opensfm.unionfind import UnionFind
from opensfm import pysfm


logger = logging.getLogger(__name__)


def load_features(dataset, images):
    logging.info('reading features')
    features = {}
    colors = {}
    for im in images:
        p, f, c = dataset.load_features(im)
        features[im] = p[:, :3]
        colors[im] = c
    return features, colors


def load_matches(dataset, images):
    matches = {}
    for im1 in images:
        try:
            im1_matches = dataset.load_matches(im1)
        except IOError:
            continue
        for im2 in im1_matches:
            if im2 in images:
                matches[im1, im2] = im1_matches[im2]
    return matches


def create_tracks_manager(features, colors, matches, config):
    """Link matches into tracks."""
    logger.debug('Merging features onto tracks')
    uf = UnionFind()
    for im1, im2 in matches:
        for f1, f2 in matches[im1, im2]:
            uf.union((im1, f1), (im2, f2))

    sets = {}
    for i in uf:
        p = uf[i]
        if p in sets:
            sets[p].append(i)
        else:
            sets[p] = [i]

    min_length = config['min_track_length']
    tracks = [t for t in sets.values() if _good_track(t, min_length)]
    logger.debug('Good tracks: {}'.format(len(tracks)))

    tracks_manager = pysfm.TracksManager()
    for track_id, track in enumerate(tracks):
        for image, featureid in track:
            if image not in features:
                continue
            x, y, s = features[image][featureid]
            r, g, b = colors[image][featureid]
            obs = pysfm.Observation(x, y, s, int(r), int(g), int(b), featureid)
            tracks_manager.add_observation(image, str(track_id), obs)
    return tracks_manager


def tracks_and_images(tracks_manager):
    """List of tracks and images in the tracks_manager."""
    return tracks_manager.get_track_ids(), tracks_manager.get_shot_ids()


def common_tracks(graph, im1, im2):
    """List of tracks observed in both images.

    Args:
        graph: tracks graph
        im1: name of the first image
        im2: name of the second image

    Returns:
        tuple: tracks, feature from first image, feature from second image
    """
    t1 = graph.get_observations_of_shot(im1)
    t2 = graph.get_observations_of_shot(im2)
    tracks, p1, p2 = [], [], []
    for track, obs in t1.items():
        if track in t2:
            p1.append(obs.point)
            p2.append(t2[track].point)
            tracks.append(track)
    p1 = np.array(p1)
    p2 = np.array(p2)
    return tracks, p1, p2


def all_common_tracks(tracks_manager, include_features=True, min_common=50):
    """List of tracks observed by each image pair.

    Args:
        tracks_manager: tracks manager
        include_features: whether to include the features from the images
        min_common: the minimum number of tracks the two images need to have
            in common

    Returns:
        tuple: im1, im2 -> tuple: tracks, features from first image, features
        from second image
    """
    common_tracks = {}
    for(im1, im2), tuples in tracks_manager.get_all_common_observations_all_pairs().items():
        if len(tuples) < min_common:
            continue

        if include_features:
            common_tracks[im1, im2] = ([v for v, _, _ in tuples],
                                       np.array([p.point for _, p, _ in tuples]),
                                       np.array([p.point for _, _, p in tuples]))
        else:
            common_tracks[im1, im2] = [v for v, _, _ in tuples]
    return common_tracks


def _good_track(track, min_length):
    if len(track) < min_length:
        return False
    images = [f[0] for f in track]
    if len(images) != len(set(images)):
        return False
    return True


def as_weighted_graph(tracks_manager):
    """ Return the tracks manager as a weighted graph
        having shots a snodes and weighted by the # of
        common tracks between two nodes.
    """
    tracks, images = tracks_and_images(tracks_manager)
    image_graph = nx.Graph()
    for im in images:
        image_graph.add_node(im)
    for k, v in tracks_manager.get_all_common_observations_all_pairs().items():
        image_graph.add_edge(k[0], k[1], weight=len(v))
    return image_graph


def as_graph(tracks_manager):
    """ Return the tracks manager as a bipartite graph (legacy). """
    tracks, images = tracks_and_images(tracks_manager)

    graph = nx.Graph()
    for track_id in tracks:
        graph.add_node(track_id, bipartite=1)
    for shot_id in images:
        graph.add_node(shot_id, bipartite=0)
    for track_id in tracks:
        for im, obs in tracks_manager.get_observations_of_point(track_id).items():
            graph.add_edge(im, track_id, feature=obs.point, feature_scale=obs.scale,
                           feature_id=obs.id, feature_color=obs.color)
    return graph
