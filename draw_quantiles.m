
colors = [
"#1b9e77";
"#d95f02";
"#7570b3";
"#e7298a";
];

modes = ["m", "p", "t", "x"];
labels = {'shm atomic ipc', 'sockpair ipc', 'shm atomic threads', 'sema threads'};
handles = zeros(size(modes, 2), 1);

clf; hold on;
figure(1);

for m = 1:size(modes, 2)
  d = strcat("mode_", modes(m), "/");

  files = ls(d);
  for i = 1:size(files, 1)
    data = csvread(strcat(d, files(i, :)));
    handles(m) = plot(sort(data), "Color", colors(m,:), "LineWidth", 2);
  end
end

title('Processing Time inverse-CDF (4k element buffer)');
xlabel('Sample');
ylabel('Iteration Time (ns)');
legend(handles, labels);
grid on; grid minor;
