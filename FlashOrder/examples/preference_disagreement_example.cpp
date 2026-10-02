#include "hotstuff/preference_disagreement.hpp"
#include <iostream>

int main() {
  const std::vector<std::vector<int>> certificate{
      {0, 1, 2, 3}, {0, 1, 2, 3}, {0, 1, 2, 3},
      {1, 0, 2, 3}, {1, 0, 2, 3}, {1, 0, 2, 3}};
  flashorder_preference::Sorter sorter(6, 0, 0.6, 4.0);
  const auto result = sorter.sort(certificate);
  std::cout << "D=" << result.disagreement << " delta=" << result.delta
            << " clusters=" << result.clusters.size() << "\norder=";
  for (int tx : result.order) std::cout << tx << ' ';
  std::cout << '\n';
}
