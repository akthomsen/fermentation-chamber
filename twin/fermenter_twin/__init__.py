"""Fermentation-chamber digital twin runtime.

A thin runtime that observes the chamber over MQTT and runs runtime monitors
against the live signal. Built model-free in v1; the model/STL seam (predicted
state, residuals) is left explicit and empty for later work. See README.md.
"""
