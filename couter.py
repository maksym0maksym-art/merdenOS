import os
from pathlib import Path

ALLOWED_EXTENSIONS = {
    '.py', 'asm', '.cpp', 'h', '.c'
}

IGNORE_DIRS = {
    '.git', 'pycache', 'venv', 'env', '.venv', 
    'node_modules', '.idea', '.vscode', 'build', 'dist'
}


def is_ignored(path):
    """Перевіряє, чи міститься в шляху папка, яку треба ігнорувати."""
    for part in path.parts:
        if part in IGNORE_DIRS:
            return True
    return False

def count_lines(filepath):
    """Рахує загальну кількість рядків та порожні рядки у файлі."""
    total_lines = 0
    blank_lines = 0
    try:
        with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
            for line in f:
                total_lines += 1
                if not line.strip():  
                    blank_lines += 1
    except Exception as e:
        print(f"Помилка читання {filepath}: {e}")
    
    return total_lines, blank_lines

def analyze_project(root_dir):
    """Головна функція аналізу проекту."""
    root_path = Path(root_dir)
    
    total_files = 0
    project_total_lines = 0
    project_blank_lines = 0
    
    print(f"\n{'Файл':<50} | {'Всього':<8} | {'Порожні':<8} | {'Код':<8}")
    print("-" * 85)

    for filepath in root_path.rglob('*'):
        if filepath.is_file() and filepath.suffix in ALLOWED_EXTENSIONS and not is_ignored(filepath):
            total, blank = count_lines(filepath)
            code = total - blank
            
            project_total_lines += total
            project_blank_lines += blank
            total_files += 1
            
            rel_path = str(filepath.relative_to(root_path))
            if len(rel_path) > 47:
                rel_path = "..." + rel_path[-44:]
                
            print(f"{rel_path:<50} | {total:<8} | {blank:<8} | {code:<8}")

    code_lines = project_total_lines - project_blank_lines

    print("-" * 85)
    print(f"📊 РЕЗУЛЬТАТИ АНАЛІЗУ ПРОЕКТУ:")
    print(f"📁 Всього проаналізовано файлів : {total_files}")
    print(f"📝 Загальна кількість рядків    : {project_total_lines}")
    print(f"⬜ Порожніх рядків              : {project_blank_lines}")
    print(f"💻 Рядків коду (без порожніх)   : {code_lines}\n")

if __name__ == "__main__":
    project_directory = "." 
    
    analyze_project(project_directory)                                                                                              
