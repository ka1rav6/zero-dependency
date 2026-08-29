# Fixture only: kap never reads this file's contents, it only notices it exists.
cmake_minimum_required(VERSION 3.16)
project(simple LANGUAGES CXX)
add_executable(simple main.cpp)
