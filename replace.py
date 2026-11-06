"""
Remove 'k' prefix from patterns in .hpp and .cpp files.
"""

import argparse
import re
from pathlib import Path

def list_files(directory):
    """
    List all .hpp and .cpp files in the given directory and its subdirectories.

    Args:
        directory (str): The path to the directory to search.

    Returns:
        list: A list of Path objects representing the .hpp and .cpp files.
    """
    path = Path(directory)
    exclude = re.compile(r"^.*(build|\.git|vendor|\.vscode|\.idea|\.vs|\.mypy_cache|\.pytest_cache|\.tox|\.eggs|\.venv|__pycache__|node_modules).*")
    return [f for f in path.rglob("*") if f.is_file() and f.suffix in [".hpp", ".cpp"] and not exclude.match(str(f))]

def remove_prefix(pattern, prefix_string, files):
    """
    Remove the prefix string from the given pattern and lowercase the next character in the pattern for each file.

    Args:
        pattern (str): The pattern to modify.
        prefix_string (str): The prefix string to remove.
        files (list): A list of Path objects representing the files to modify.
    """
    for file in files:
        repl_content=[]
        with file.open("r", encoding="utf-8") as f:
            for line in f:
                if re.search(pattern, line):
                    print(f"Found pattern in file: {file}")
                    repl = re.sub(pattern, lambda m:
                                  m.group(0).replace(prefix_string, "", 1)
                                  , line)
                    new_pat = pattern.replace(prefix_string, "", 1)
                    repl = re.sub(new_pat, lambda m:
                                  m.group(0)[0] + m.group(0)[1].lower() + m.group(0)[2:]
                                  , repl)
                    print(f"Original line: {line.strip()}")
                    print(f"Modified line: {repl.strip()}")
                    repl_content.append(repl)
                else:
                    repl_content.append(line)

        if repl_content:
            # Write the modified content with LF line endings
            res = "".join(repl_content).encode("utf-8").decode("utf-8").replace("\r\n", "\n")
            if not res == file.read_text(encoding="utf-8").replace("\r\n", "\n"):
                with file.open("w", encoding="utf-8") as f:
                    f.write(res)


def remove_k_prefix(files, dry_run=False):
    """
    Remove the 'k' prefix from patterns in the given files.

    Args:
        files (list): A list of Path objects representing the files to modify.
    """
    # exclude a name followed by k
    pattern = r"[\ ()]+k[A-Z][A-Za-z0-9_()[\]:;<>\{\}]+"
    prefix_string = "k"
    if not dry_run:
        remove_prefix(pattern, prefix_string, files)

def main():
    arg = argparse.ArgumentParser(description="Remove 'k' prefix from patterns in .hpp and .cpp files.")
    arg.add_argument("directory", type=str, help="The directory to search for .hpp and .cpp files.")
    arg.add_argument("--dry-run", action="store_true", help="Perform a dry run without modifying files.")
    args = arg.parse_args()

    files = list_files(args.directory)
    print(f"Found {len(files)} .hpp and .cpp files in '{args.directory}'.")
    remove_k_prefix(files, dry_run=args.dry_run)


if __name__ == "__main__":
    main()