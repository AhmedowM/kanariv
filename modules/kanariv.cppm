// Umbrella mirrors include/kanariv.hpp (all libraries).
module;

/**
 * @file kanariv.cppm
 * @brief Umbrella module for kanariv.
 * @ingroup kanariv
 * @details Thin mirror of `include/kanariv.hpp`, which holds the canonical
 * layer, threading and error-handling documentation. See also `kanariv.ipc`,
 * `kanariv.service` and `kanariv.client`.
 */
export module kanariv;

export import kanariv.version;
export import kanariv.types;
export import kanariv.utils;
export import kanariv.media;
export import kanariv.player;
export import kanariv.db;
export import kanariv.engine;
export import kanariv.config;
export import kanariv.ipc;
export import kanariv.service;
export import kanariv.client;
export import kanariv.app;
